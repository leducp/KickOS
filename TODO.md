<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS TODO

The granular, actionable items: every open entry and every open blocker in full. The milestone
plan is `roadmap.md`, the one-screen state `STATE.md`, and the board and console readiness matrix
`docs/m2-readiness.md`. Check an item off as it lands. At its milestone's merge a closed entry
leaves this file and git keeps it: `git log -p -S'<a phrase of the entry>' -- TODO.md` finds the
commit that removed it.

A commit hash in an older entry may name a commit a squash destroyed: read the entry for what it
states, never for the hash (`STATE.md`, *A commit hash is never a record*).

## M10 -- static composition, and an init provider that stays

`roadmap.md`'s M10 section owns the shape and the ledger: one composition file, one host tool,
one emitted table, one init, written test-first against
[`examples/composition/`](examples/composition/). The design those add up to is
[`docs/design-m10-composition.md`](docs/design-m10-composition.md), which also lists what is still
open. KISS there means the simple solution, not the
one closest to what exists: the scattered mechanisms below are replaced and deleted, never
wrapped. The kernel may change where the work needs it, a wrong errno for instance, but changing
it is not the point.

**WHAT M10 DELETES, SO THAT ONE WAY IS LEFT.** The per-board `kos_service_list` arrays and the
`kos_service_cfg` kinds they carry. The per-board pin-map tables. The service-list selection knob and
pin-map Kconfig selections. The app authority macro and
its default definition's source file. The bring-up choreography apps re-roll by hand, of
which `user/apps/f411disco/f411spi/main.cc` and `user/apps/xmc4800-relax/xmcssc/main.cc` are the
worked examples. Line numbers claimed by number in packaged drivers. M10.0 confirms the list; the
milestone does not close while any of them still composes a system.

- [ ] **M10.0 AND EVERY STAGE AFTER IT: GENERAL-PURPOSE USE STAYS POSSIBLE.** Ruled by the
      maintainer on 2026-09-28: M10 may not block KickOS growing into a general-purpose system,
      a desktop of its own included. The shape is a nested init -- a task of the boot composition
      that is itself an init for a dynamic subsystem, launching programs at run time with the
      capabilities it holds. Each stage is checked against four rules: tasks with the required
      rights can still create tasks and map memory at run time, with no new "is the init" test
      (the right to create tasks becomes a separate authority in M10.1); death and readiness reports
      go to whoever created the task, not to the boot init by name; runtime objects are mapped
      at run time; and table indices represent the supported configured system size without a
      fixed system-wide ABI ceiling. Pool limits stay configurable, while per-operation bounds
      such as the spawn grant count may stay small. `roadmap.md` and
      `docs/design-m10-composition.md` carry it.

- [ ] **M10.1: THE KERNEL SHARE -- EVERY M10 CHANGE TO THE KERNEL ABI, AS ONE STAGE.** Re-cut on
      2026-09-28 (maintainer): the kernel mechanisms the init needs are independent of the
      composition, so they land first, each with its own self-test arms and no composition, and
      reviewable together as the one ABI change M10 makes before the freeze. They are:
        - deaths and first receives raised on the creating task's notification, with the
          generation the readiness rule checks (the M10.0 readiness and restart items);
        - the right to hand out an endpoint's receiving without being a receiver, the errno
          split it enables (`-KOS_EAGAIN` while a restart may come, a new `-KOS_ECONNREFUSED`
          once none will, `-KOS_EPIPE` unchanged for a server that died holding the request),
          and every caller that tests for `-KOS_EPIPE` updated;
        - a window list in spawn (the item below);
        - on a translating board, the kernel choosing where a window sits and recording it, and
          `kos_window_addr` asking it (the M10.0 item);
        - the x86 port grant -- a per-task I/O permission bitmap loaded into the core's
          task-state segment on each switch -- and **`kos_port_reg_write(base, offset, value)`**,
          a byte-wide write beside `kos_periph_reg_write`, whose store is one aligned 32-bit word
          in a memory window and cannot serve a one-byte port (found by the external audit). Its
          possession check is the port counterpart of the memory one: a port grant covering
          `base + offset`, and the register on the kernel's allowlist with the value inside its
          mask, the CMOS index's withholding bit 7, the NMI mask;
        - the separate task-creation authority (the item below), and **the authority word
          widened**: it is eight bits in `kos_thread_params`, the new bit leaves one free, and a
          small fixed ceiling in the ABI is what the general-purpose rule forbids.
      M10.3 runs alongside it; M10.4 needs both.

- [ ] **M10.1: A TASK HOLDS SEVERAL DEVICE WINDOWS.** Ruled by the maintainer on 2026-09-28: the
      one-window limit goes, since it stops a task driving a DMA engine and its peripheral, or a
      few devices directly without a server. Today a spawn carries one window and the kernel
      records one per thread (`Thread::dev_base`, `dev_size`), read by the one-holder check and by
      the `kos_periph_enable` and `kos_periph_reg_write` gate. The spawn carries a window list,
      the thread keeps a small array bounded by the protection unit's region budget, the
      one-holder check runs per window, and the peripheral seams accept any window the caller
      holds. `docs/design-m10-composition.md` carries the detail.

- [ ] **M10.1: A SEPARATE TASK-CREATION AUTHORITY.** Ruled by the maintainer on
      2026-09-28, reversing the draft's "a later decision", and placed in the kernel share with every other M10
      ABI change: M10 cannot close without it, because an
      authority added after the ABI freeze breaks the ABI, and M11 and M12 build on the finished
      shape. Today task creation is ungated by authority and gated by creatorship alone
      (`KOS_SYS_TASK_CREATE`), so a task is free to mint tasks, the hole the object-budget item
      above has recorded since M8.5: one unprivileged caller seats a thread in every task slot
      and empties the pools. Creating a task requires an authority the composition grants; the
      boot init holds it and hands it to a nested init. Settle whether it is a bit in the
      authority word, which has two free, or a capability; whether it also covers placing a
      thread in another task, one of the deferred questions; and what a task's own threads
      need, a task's concurrency being bounded by its budgets already.

- [ ] **M10.5: THE CLEANUP.** Write a chip file for every chip the fleet builds, not only the eight
      the M10.0 draft covers, and generate the kernel's chip headers (`chip_mmap.h`, `irq.h`,
      `arch_reserved_blocks`) from them, deleting the hand-written ones. Give every board its
      minimal default composition, build the partition's compositions together (a partition gate's
      assignment derived, a device two nodes grant refused, a region cached across nodes unless
      accepted), and link `KickOS::system_default` in the out-of-tree examples CI builds
      (`examples/oot-app`, `examples/oot-mcu-app`; **DONE AT M10.5.6**, their gates running them
      where an emulator runs). Move every board and app onto compositions (the common apps but the
      selftest and `ampping`: **DONE AT M10.5.10**), packaged drivers taking their lines from the
      composition rather than by number, and delete the mechanisms listed above (**DONE AT
      M10.5.13**). The four apps whose child never exits (`initdemo`, `tele_pingpong`,
      `drvdeath`, `rootfault`) declare their ending explicitly (**DONE AT M10.5.10**, `ends:
      main`). Run the fleet sweep and the silicon witnesses
      against the result. Owed: esp32-wroom-smp silicon selftest (5.14), the witness `lx6smp`'s
      deletion in M10.5.11 rests on (`S32C1I` from both cores, `PRID` per core); its capture is
      named in `docs/reference/boards.md`. Owed: the pizero2350-amp2 partition's ACCESSCTRL gate
      witness (5.14), a silicon capture `tests/integration/check_pizero_amp_gate.sh` passes,
      taken by `AMP_PARTITION=1 APP=ampping_n0 VARIANT=amp2-n0 tools/bench/bench.sh pizero2350`;
      until then the judge holds only its planted capture.

- [x] **M10.6.2: APPS BUILT BY BOARD NAME, AND APPS THAT BUILD AND NEVER RUN (found 2026-10-06, cut from
      M10.5 for scope).** The emulator gates are keyed on facts and `board_predicates` refuses a
      board name around one; two neighbouring gaps were found and left open.
      - `user/apps/common/CMakeLists.txt` still selects apps by board name: `fpclass` (rxv3 or
        `qemu`), the `cxxtest`/`cxxterm` list, `tele_pingpong`, `fp_switch` (the FPU boards),
        `gpioblink`, `blink`, `usbcdcwit` and `clockretune`. Each has a fact to key on: the newlib
        profile (nano prints no double), the board's heap (microbit's 1 KiB runs out under
        `cxxtest`'s vector arm, and its RAM fits no larger heap), `KICKOS_TELEMETRY_ON`,
        `KICKOS_MFLOAT_ABI`, a `systems/<board>.yaml` the app ships, and whether the chip's own
        sources define `arch_diag_led_set` / `arch_cpu_clock_set`. `board_predicates` should then
        read this file too.
      - Apps that build on an emulated preset and that no test boots: `stackguard` on x86_64 (its
        probe line prints the address with `%x`, which truncates an x86_64 user address),
        `aspacefault` on rv64imac, `aspaceufault` on armv8a, and the SMP arrival, doorbell and
        every-core-thread gates on x86_64 SMP (q35 prints no `# smp:` banner, and QEMU's x86 model
        names no core in an interrupt event, so the second channel would read the execution log).
        A per-build gate should refuse an image no test boots unless it states why. Running every
        capture-judged image under its emulator on the way also found premises that do not hold:
        `initdemo` spawns its sink on a stack main allocated (one shared address space only),
        `mpu_fault` grants memory to one thread of a task (no translating backend can),
        `objbudget` assumes the endpoint pool holds more than one task's budget (false on the
        arm64 AMP and `benchsmp12` postures, where the init holds endpoints first), the sim links
        the host's terminate handler (`cxxterm`), `specfault`'s judge reads captures only, and the
        sim maps no guard under a thread's stack (`faultsurvive_ovf` overruns `main`).
      **LANDED:**
      - Each app is selected by the fact it needs, and `board_predicates` refuses a board name in
        this file too. The name lists were short: `fpclass`, `cxxtest`, `cxxterm`, `fp_switch`
        and `blink` now build on every board their fact admits.
      - A configure refuses an image no test of that build boots, unless it states why
        (`kickos_unbooted`, `kickos_inapplicable`, `kickos_human_judged`). `stackguard` prints
        `%lx` and boots on x86_64; `aspacefault` boots on rv64imac and `aspaceufault` on armv8a;
        `hello_c`, `stress`, `specfault` and the armv8a and x86_64 exit, root and reclaim images
        boot too. The x86_64 SMP arrival, doorbell and every-core gates state why: the x86_64
        SMP selftest holds those claims.
      - Where a premise fails the image is not offered: `stackdepth` without the kernel-stack
        line, `mpu_fault` on a translating backend, `objbudget` on the AMP-port postures and
        where the budget reaches `KICKOS_MAX_SPAWN_GRANTS`, `trapnest` without an MPU,
        `faultsurvive_ovf` and `cxxterm` on the sim. `initdemo`'s premise holds wherever it is
        built.

- [ ] **M10.6.4: THE SELFTEST ORDERED BY EVENTS.** Arms still order threads by sleeping
      (`EP_CALL_SETTLE_NS` and the like) and fail under a loaded host: a sleep is not an order. Each
      such arm orders by priority, a semaphore or a mark instead, and releases what it created on
      every path (`ArmHold`), so one failing arm cannot starve the rest; the census between arms
      already names a leak at the arm that made it.

- [ ] **M10.7: THE EXIT RECORD.** Reconcile `roadmap.md`, `docs/reference/architecture.md`,
      `docs/reference/invariants.md` and `STATE.md` against what shipped, and record what the green
      runs do not say.

**DEFERRED UNTIL A CONSUMER NEEDS ONE, AND NOT WORK OF THIS MILESTONE.** The seal, a
create-suspended spawn, and an authority for placing a thread of another task, a dynamic mint and
starting a core. None may be answered with "root" when it comes, nor in a way that suits only the
boot init: a nested init is the consumer most likely to reach them first. The temporal half is out
by ruling and sits in `roadmap.md`'s `Later`.

## Scheduler options for later (maintainer, 2026-10-03)

- [ ] **MORE THAN 32 PRIORITY LEVELS, AS AN OPTION (maintainer, 2026-10-03).** The ready structure
      is one `uint32_t` bitmap, one bit per level, picked with one count-leading-zeros
      (`kernel/sched/policy_fifo_rr.cc`), which fixes 32 levels on every board. When a system needs
      more, the shape is a two-level bitmap: a summary word whose bit i says word i is non-empty,
      over eight 32-bit words for 256 levels (the summary can be a `uint32_t` at the same cost,
      which would reach 1024). Pick is two scans, still constant time; making ready sets the level
      bit and the summary bit; the last thread leaving a level clears its bit and the summary bit
      when the word empties. It is an OPTION, off by default, so a slow chip does not pay for a fast
      one: on ARMv6-M and rv32imac each scan is libgcc's software clz, so the pick roughly doubles
      there. The range stays structural (declared in `cmake/sched_geometry.cmake`, exported in the
      manifest), so a composition's priorities are checked against the build's range either way.

- [ ] **A FAIR-SHARE BAND, AS AN OPTION OF ITS OWN (maintainer, 2026-10-03).** Today every level is
      strict priority, FIFO or round-robin inside a level, which is Linux's real-time half
      (`SCHED_FIFO`, `SCHED_RR`) and nothing of its default fair-share class (CFS, then
      EEVDF), where `nice` sets a weight and a thread gets CPU time in proportion to it.
      **Trigger: the first general-purpose subsystem** (a shell, a nested init launching
      programs, the dynamic-payload case), where many non-real-time tasks share the CPU and
      hand-assigned priorities would starve each other. Shape (maintainer, 2026-10-03): the
      band takes the lowest level of the normal 32-level range, or, with the two-level
      structure above, its FIRST word (the lowest 32 levels) while every other word is
      real-time (eight words give 224 real-time levels, a `uint32_t` summary over 32 words
      gives 992), all strictly above it, as Linux's real-time classes sit above its fair
      class; whether the band's 32 levels carry weight classes or act as one queue is decided
      when it is built; threads in the band are picked by least weighted runtime instead of
      FIFO, runtime is charged at each switch from timestamps (the scheduler is tickless), and
      `nice` sets the weight. Cost: a per-core ordered queue for the band and an accounting
      step per switch, paid by threads in the band only. No starvation guard (maintainer,
      2026-10-03): a real-time thread that never blocks starves the band, as Linux with its
      real-time throttling disabled, which heavy users of real-time Linux do. Priority
      inheritance is the existing mechanism across the boundary: a band thread boosted into a
      real-time level keeps its runtime account frozen and resumes with it when the boost
      ends. Multicore (maintainer, 2026-10-03): each core's band keeps its own virtual clock,
      and a migrating thread's account is simply reset to the destination core's clock rather
      than carried as a lag, since a thread migrates only to a core free to run it, where
      there is little or nobody to be unfair to; a thread waking from a long sleep is placed
      at its core's clock the same way. An OPTION of its own, off by default and independent
      of the wider range. Read beside the pluggable EDF and rate-monotonic policies in
      `roadmap.md`'s Later section.

## M10.6 -- the cleanup pass (maintainer, 2026-10-06)

M10.6 cleans up before the exit record; its steps are in `roadmap.md`. The entries below, and the
ones elsewhere in this file tagged `M10.6.<n>`, are its work. M10's tail (maintainer, 2026-10-03)
became this milestone.

**The rule (maintainer, 2026-10-06): at the same features, less is more.** M10.5 grew the product
(kernel, arch, system, user/src, cmake) by about 3k lines and the tests and tools by about 31k;
today `tests/static` (44k), `tests/unit` (50k), `tests/integration` (17k), `tools` (22k) and the
selftest app (23k) together weigh what the kernel, arch, user and system do. The DRY inventory's own
ceiling is about -7.5k, because a fold removes a copy and not a feature of the apparatus. The larger
cuts are a change of shape: a gate reduced to its rule with its harness paid once, a hand list
derived so the gate comparing it has nothing left to compare, lint rules in one table, one
kernel-under-test library for the unit suites, and the closed records out of the tree. So the steps
run in the order below, numbered as they run: the cuts that are read-only on the host before the
folds that need silicon, and nothing folded that a later step would delete. A second copy of a fact
exists only where the first cannot be read at the time of the check (the Python region rule,
`panic.ere`, `kicktrace.py`); where both copies are hand-maintained text, one is derived and its gate
goes with it. Each step's close records three numbers, lines of product, lines of tests and tools,
and their ratio; no gate is written over them.

- [x] **M10.6.0: THE MISSES FOUND AFTER M10.5 MERGED.** Each verified on the tree, each fixed with
      its arm before the cleanup moves code. Only the first is M10.5's; the rest are older.
      **LANDED**, each with the arm that reddens without it:
  - The C6 APM test's kernel blocks are read from the generated `chip_mmap.h` and include
    `gpio_matrix`; `Ree0PastNode0sGrantsIsCaught` now names the one block the open GPIO row
    covers.
  - `KOS_SYS_SEM_WAIT` answers `-KOS_ECANCELED` to a cancelled waiter: `sem_post` writes the
    handed waiter's result, and the arm reads it after the resume barrier, outside the lock, as
    `mutex_lock` does (`park_result`'s `a_cancelled_semaphore_wait_returns_ecanceled`).
  - `domain_resolve` has no sign test; the null domain's handle is refused by its all-ones
    index. `tests/unit/grantnocache` compiles the real `domain.cc` and ages a slot past 0x8000.
  - `domain_for` asks the memory type once, ahead of both arms, so a region backend answers
    `-KOS_ENOTSUP` as the ABI says (`DomainAdmit` in the same suite).
  - `console_tx_write`, `write_unbuffered`, `enqueue_locked`, `wait_space` and
    `console_chip_writable` are deleted with their 395 indirect-site records. The masked-window
    and publish-handoff suites drive `console_tx_insert_line`; the cases that described only
    the burst producer's chunk gaps went with it. The room and record fixtures stage a lower
    writer preempted in the gap after its insert, the one time a backend with no TX interrupt
    holds its bytes. **Found on the way, for M10.6.5:** the live insert re-reads no ownership,
    so an unbracketed producer after a publish writes the driver's device; that is the
    direct-arch-print case below (decision 3), and its arm went with the dead re-check.
  - `tests/lib/gate.sh` derives `KOS_FAULT_RE` from `panic.ere`, and the panicgate judge reads
    it; `check_app_judges.sh` plants the arm64, rv64 and x86_64 banners.
  - `run_image`'s native branch charges `boot_bound` like the QEMU one, and
    `kickos_boot_timeout` (renamed from `kickos_qemu_timeout`) reads `SIM_TIMEOUT` on the sim, so
    `fault_dump`, objbudget, rootauth, the reboot gate and selftest take a derived timeout and a
    short one fails with a finding; `hello_demo` and `telemetry_ring_wrap` sit above their
    Python scripts' own waits.
  - `KOS_SYS_IRQ_UNMASK` refuses a kernel-owned line, with the selftest arm beside the claim
    and inject refusals.
  - With them: `usbcdcwit` zeroes its request, `consoledemo` prints the `ERROR:` its judge
    reads, and the dead `rp2040/regs/xip_ssi.h`, `esp32c6/regs/usb_serial_jtag.h` and simuart's
    `tx_idle`/`tx_irq_enable` are gone.
  - **Open, decision 3:** direct arch prints after a publish (boot and doorbell-timeout paths
    only) still write the driver's UART.
  - **Close (2026-10-06):** product 135,838 to 135,610 lines, tests and tools 179,051 to 178,693,
    ratio 1.31 (product: `arch`, `kernel`, `lib`, `include`, `system`, `user/src`,
    `user/include`, `platform`, `boards`, `cmake`, the root `CMakeLists.txt` and `Kconfig`; tests
    and tools: `tests`, `tools`, `user/apps`). Swept: the sim whole, and one to three presets of
    every family with their image or host gates.

- [x] **M10.6.1: THE ARCHIVES TRIMMED.** First, because every later deletion pays
      `check_doc_names.sh` for each name it removes while the archive is still in the tree. Records
      of merged milestones in the archive directory, the closed entries in this file and finished
      sections of `STATE.md` leave the tree, since git history keeps them; what is still cited
      moves to where it is cited, and every link is updated. **LANDED**:
  - The archive's 441 files (about 93k lines) are gone. A page cites one as archived `<name>`,
    and `docs/README.md` says how to find it in history, with no commit hash, since branches
    are squashed along the way. The one capture a design page leaned on, the RX DPFPU census, moved
    into `docs/design-m10-toolchain.md`.
  - `check_ascii.sh` keeps one byte exemption, `LICENSE`, and its NUL-exemption mechanism is gone:
    a byte-exempt file holding a NUL is refused as binary. `check_spdx.sh` drops the four file
    classes only the archive held, and `check_panic_banners.sh` reads no `docs/` source.
  - This file keeps its open items (267) and the milestone in progress; the closed entries of
    merged milestones and the sections that held only them went, and so did seven sections that
    were finished records written without a checkbox. Each citation of a removed entry carries its
    fact or names the code that holds it (G-06 had landed in the root `CMakeLists.txt`).
  - `STATE.md` keeps its standing sections and M10.5's; the sections for M7.11 through M10.4 went.
    The M8.5 bisection the roadmap cites now sits in the roadmap.
  - **Close (2026-10-07):** `TODO.md` 15,096 to 4,230 lines, `STATE.md` 4,118 to 1,170, the
    archive 93,450 to 0. Product and tests and tools are unchanged by this step.

- [x] **M10.6.2: THE GATE AUDIT, SIZED BEFORE IT STARTS.** The inventory sized the DRY items and
      left the audit unsized; the audit is where the mass is. Its first deliverable is a table over
      the 116 static gates and the 79 unit suites answering four questions each: which bug class it
      prevents; whether it has ever fired on a real change (`git log -S` on its name); whether the
      compiler, a linker-script `ASSERT` or `-Werror` can hold the rule instead; whether the build
      can generate the thing it compares, so the copy does not exist. The answers map to delete,
      replace, derive or keep, and the table is the step's budget. Then the shape change:
      - The harness once. About 77 of the 116 gates carry their own planted controls, 23 walk
        `git ls-files` themselves and 16 re-implement the comment stripper. Nearly all source
        `tests/lib/gate.sh`, but its two control helpers serve 30 of them. `check_cpu_id_fold.sh` is 436 lines, of which about 75 are its rule
        and about 290 its controls and refusals. The controls, the corpus walk with its floor and
        the dead-reader refusal live once in `gate.sh` (`ctl_fires`, `ctl_quiet`, `corpus`), and a
        gate is its rule and one planted file. The feature stays: a gate still proves it can fire
        and still refuses a corpus it could not read.
      - The lint gates become one rule table: `check_ternary` (646 lines around `?:` and
        `for (;;)`), `check_ascii` (656), `check_spdx` (421), `check_include_guards` (371),
        `check_c_headers` (473), `check_public_headers` (371), `check_doc_names` (695),
        `extern_c_linkage`; one driver, one stripper, one corpus.
      - A hand list derived from the tree goes with the gate that compared it:
        `trap_redzone_indirect.txt` (1820 lines), `trap_redzone_roots.txt` (756),
        `console_reach_roots.txt`, `app_stack_roots.txt`, `_selftest_seat_skips`, the ci.yml name
        families. The inventory's section 6 keeps several as "the independent oracle"; an oracle
        costs a copy, a gate and the gate's controls, and is kept only where the primary cannot be
        read at check time.
      - Shared parsing (macros under a TU's flags, comments, CMake shapes, the record grammar) in
        one library; the gate that races a configure; apps built by board name or never run;
        presets CI does not build; the bench fleet run across boards in parallel.
      A new fix's arm extends a gate or is a unit test; a new static checker is written only for a
      class likely to recur, and it starts from `gate.sh`.
      **SIZED (2026-10-07).** 124 gates (39k lines) and 80 suites (50k) read; about 25k gate lines
      and 4.5k suite lines removable. 11 gates show a real catch; 42 caught only their own planted
      controls. No suite goes: every one holds something nothing else holds, so its savings are
      shared seams. Taken in waves, least risk first, each gate going only once its replacement
      has been shown to refuse the gate's own planted defect:
      1. delete (7), fold (10), unit (6), and the harness shrink into `gate.sh` (22): about 7k.
      2. derive (16), the hand lists generated from facts the build has: about 4k.
      3. replace with a build mechanism (23), a flag, linker `ASSERT`, `static_assert`, configure
         check, or seams linked whole: about 8.6k, product behaviour unchanged.
      4. the shared unit seams: one console seam, one parameterised K-seam, one region and one
         translating fake, one user-side `kos_*` fake: about 4.5k.
      5. replace with a product type or token (15): about 5.4k, kernel and arch code. **Ruled
         (maintainer, 2026-10-07): with M10.6.3's DRY pass**, whose silicon witnesses it needs.
      **Ruled (maintainer, 2026-10-07):** a replacement is proven once and keeps no test of its
      own: the commit that deletes a gate plants that gate's defect, shows the build refusing it
      with the error text, and reverts. A flag or link mode is set where inheritance carries it to
      a library added later (the kernel's interface target), never on a list of libraries. The
      per-core bench stamp gate goes, its defect needing a lock the kernel does not have; the
      Xtensa stamp gate becomes a discard of wrapped deltas in `kickos_bench_switch_done`; the
      x86_64 entry gate was to become `-mcld` once no assembly `memcpy`/`memset` links into the
      kernel half, and **stays**: none does, but under `-mcld` GCC 16.2 still lowers `kmemcpy`'s
      and `kmemmove`'s byte loops to a `movsb` with no `cld` before it, so the flag does not hold
      the rule and the entry's `cld` is still the whole protection.
      **LANDED**, waves 1 to 4, each gate going in the commit that showed its replacement refusing
      the gate's own planted defect, or its images byte-identical:
  - Deleted or folded: the five above, `c6_clock_first` (the HP `SystemCoreClock` boots at the
    ROM's rate), `x86_64_link_order` (85 of 86 x86_64 gates pass with the order changed), and
    `c_headers`, `lx6_atomctl`, `kernel_runtime`, `console_reach`, the x86_64 no-GOT selftest
    and `kconfig_reach` into the gate beside them. `gate.sh` gained the image-body, shell-corpus
    and floor helpers those share, and every gate's Python writes no bytecode into the sources.
  - Moved to unit tests: `smp_predicate` and `isolated_cores` (`cmake -P` cases), `stamp_lines`,
    `kconfig_gen`, `trap_redzone_walk` (Python unittests beside their tools), `boot_bounds` and
    `capture_matchers` (`tests/lib/check_gate.sh`).
  - Shrunk to their rule (20): `ternary`, `ascii`, `spdx`, `include_guards`, `irq_line_op_sole`,
    `syscall_return_codes`, `sweep_checkout`, `shell_special_names`, `tlbi_shareability`,
    `panic_stack_seat`, `arm_read_tp`, `amp_slot_snapshot`, `amp_no_xip_pin`,
    `appdata_no_kernel`, `x86_64_no_vector`, `esp_tx_latch_ack`, `dash_punct`, `doc_names`,
    `app_judges`, `death_stack_seating`.
  - Derived: the app-stack `presets=` lists, the trap and console-route registration (each trap
    class now in its own header), the seatless-doorbell skips, the geometry headers, one
    `.appdata` linker macro, one block-redirect helper for the seven carving backends.
  - Replaced by the build: `cpu_id_fold`, `bench_a53_pmcr`, `riscv_kernel_gp`
    (`--no-relax-gp` on the kernel's interface target), `riscv_no_smalldata`, `riscv_kernel_wx`,
    `rv32_trap_gp_anchor`, `rp_node_vectors`, `app_heap_align`, `kernel_ctor_placement`,
    `tls_carve_link`, `amp_elf_agree`, `object_budget_asserts`, `smp_trace_builds`,
    `oot_arch_cover`, `arm64_entry_order`, and `kconsole_emit`: `kos_kconsole_write` is poisoned
    in every app, out-of-tree ones included, and each raw call states its measurement,
    `KICKOS_KCONSOLE_MEASURED(DROP|ANSWER|WAIT, buf, len)` (**ruled, maintainer, 2026-10-07**).
  - **Restored after a clause-by-clause audit (maintainer, 2026-10-07).** Each replacement
    above had been proven against one planted defect, and a review found the heap base held to
    its low bits only. The rule since: every failure case of a deleted gate, its clauses and its
    planted controls, must fail its replacement. Of about 99 cases that had stopped failing,
    all but three MOOT ones fail again, through one shared image reader
    (`tests/static/check_image_rules.sh`, ten per-arch rules, each run on a planted copy of its
    own image), a cross-node `.amp_shared` comparison in `build-partition.sh`, `app_window.ld.h`
    applied after every chip script by the build, each task budget measured against the pool
    its own arm indexes, and build rules where they hold the case. **Accepted losses
    (maintainer):** the per-core bench stamp and the Xtensa stamp's cases, by decision 23; a
    window based on `.` with a correctly sized reserve still links, and is refused once kernel
    `.bss` passes the reserve. The audit and the proof tables sit in the session reports. A
    review found three cases the audit missed, restored the same way: each `kernel_l1` leaf's
    destination, the rv32 vector's length against `KICKOS_MAX_IRQ`, and the rv64 rules over
    `hello`, `selftest` and `cxxtest` with the paired-anchor floor.
  - The shared unit seams: one postured `kickos_add_kseam`, console.cc's undefined set answered
    once in `consoleseam`, six suite merges, `uartclass` without its mock (its eight mock-only
    cases went).
  - **Found on the way:** the privileged-ctor `ASSERT` caught a constructor re-zeroing the AMP
    window on every node's reset (only the primary may clear it); it is constinit now, and owes
    its RP2350 and ESP32-C6 silicon witness. `sim_driver_death` and `sim_console_restart`
    refused their own knob trees, ctest's test list was read in 11.8 s (`sim_host_gate_boots`
    against 30), and the always-built SMP trace arm joined the trap depth graph as if an image
    held it: all fixed.
  - **Refused by their proofs, gates kept:** `riscv_kernel_apphalf` (with relaxation off ld
    still links the cross-half reference, rewriting `auipc` to `lui`); the C++ half of
    `public_headers` (CMake's header sets passed three of four planted headers); `ipi_fence` (a
    seq_cst fence turns RP2350's `dmb sy` into the reserved `dmb ish`); `atomic_rmw` (rxv3 links
    the RMW under `clrpsw i`; only armv6m refuses); `bench_e2e_publish` (only its acquire/release
    half is in the type); `doorbell_isb` (goes with M10.6.3's shared service body); the
    `panic_stack_seat` cut to two backends (see below); the `app_judges` row dedup and derived
    OWED; the `doc_names` and `dash_punct` corpus narrowing; `panic_banners`,
    `preset_defconfig`, `chip_kconfig` and `chip_protection`, `ci_named_lists` (a label cannot
    require a test to stay registered), the selftest demand table, the runtime starved-arm
    record, one banner row table, and the ST-Link regexes from `diag.h` (each reads a copy no
    derivation reaches: silicon captures, presets read before configure, Kconfig read with no
    build directory, the bench host).
  - **Left open:** the region, translating, two-core and user-side unit fakes and initwalk's
    bridge (the copies differ in behaviour, not text); linking the seam and class backends whole
    (all 19 chip scripts place kernel sections by archive name).
  - **Close (2026-10-07):** product 135,617 to 135,677 lines, tests and tools 178,560 to
    163,575, ratio 1.32 to 1.21; `tests/static` 43,555 to 28,664, `tests/unit` 49,820 to
    48,776. Swept: the sim whole with UBSan, all 91 presets configured, and one to three presets
    of every family built with their gates.
- [ ] **M10.6.3: THE PANIC GATE DOES NOT WITNESS THE PANIC STACK SEAT (found in M10.6.2).** With
      both the interrupt mask and the move onto the panic stack deleted from the armv8a panic
      entry, all five qemu-arm64 panicgate cases still pass, so only `check_panic_stack_seat.sh`
      holds the seat, on all eight backends. A panic arm that reads its own stack pointer against
      the panic stack would make the emulator gate witness it.

- [x] **M10.6.2: PRESETS NO CI JOB BUILDS.** Eight visible presets appear nowhere in `ci.yml`, and
      no record says any was left out on purpose. Build each in a job, or record why not.
      **LANDED:** the arm64 `benchsmp2`, `benchsmp12` and `benchgicv3`, `qemu-riscv64-benchsmp2`,
      `esp32-wroom-benchsmp` and `qemu-x86_64-smp4`, `-smp8` and `-smp12` build in the `bench`,
      `xtensa` and `qemu-x86_64` jobs with their host gates, about 2.5 minutes each here.

- [ ] **M10.6.3: THE DRY PASS.** After the audit, so that no copy is folded inside a gate the audit
      deletes and no name a surviving text gate pins is moved twice. The inventory taken after
      M10.5 (arch, kernel and userspace, build and tools, and every static assert classed) lists
      each copy with its single form, size, risk and witness; the pass re-ranks it by lines removed
      per unit of risk: the host-only items first (the unit seams as one kernel-under-test library
      with one fake arch, where 51 seam, fake and stub files total 9.2k lines and 30 `CMakeLists`
      name kernel sources 232 times; the ABI vocabularies; the handle codec; the park unwind; the
      app helpers), the emulator-witnessed arch folds next, and the silicon-only folds (the reset
      tails, the pin guard on every chip, the semihosting chips, the F3/F4 GPIO) on M10.6.6's bench
      passes.
      **With it (maintainer, 2026-10-07), the gate audit's fifth wave:** the 15 gates a product
      type or token replaces (a lock-held token on the caller-held entries and the inject path,
      `KernelCore`, `LandedHz`, `ParkToken`, a seq_cst-only handshake type, `LineCell`, a frame-pool
      passkey, one seqlock type, the doorbell's fused take-and-resched and one shared service
      body), about 5.4k lines; each gate goes in the commit that makes its defect unrepresentable.

- [ ] **M10.6.5: THE CONSOLE READ THROUGH.** The console took ruling (c), non-blocking stdout, the
      dark window, whole fault-record lines, kernel waits, the AMP claim and the console task with
      no stdout in one milestone. Write its contract once, in `docs/reference/console.md`, and cut
      what does not serve it. Start from M10.6.0's finding: `console_tx_insert_line` re-reads no
      ownership after a publish, so whether a producer outside `console_emit`'s bracket may exist
      at all is the first question.

- [ ] **M10.6.4, FIRST: NO SELF-TEST CODE IN THE KERNEL (maintainer, 2026-10-07).** The code under
      test must be the production code. `KICKOS_ENABLE_SELFTEST` gates 152 blocks, about 4,300
      lines in `kernel/` and 820 in `arch/`, and 63 of the 86 defconfigs turn it on, every
      emulator base and bench configuration among them, so the kernel the emulator gates and the
      bench run is not the one a production image links. The blocks, worst first:
      - Configuration: `KICKOS_MAX_SPAWN_GRANTS` defaults to 9 with it and 6 without (the spawn
        frame the trap gates measure differs), and `arch_reboot` exists only with it
        (`KICKOS_SHUTDOWN_TO_BOOTLOADER`).
      - Trap and fault control flow: rv32 `switch.S` calls the nested witness on trap entry, the
        armv7m fault handler returns early on a caught probe, the fault paths report the
        trap-stack witness, virt_rv64 runs the doorbell self-check at boot.
      - Fault injection in production functions: `frame_pool_fail_in`, presync window drops
        (`drops_left`), the far-reply blind (`endpoint_far_blind_*`), `arch_irq_inject` inside a
        presync window, and the record fields they need (`PresyncRecord` changes layout).
      - Counters on hot paths: TLBI issued and elided, doorbell served and initiated, IRQ
        windows, GIC deferrals, acquire pairing, locked pages, peer release hits, some under an
        IrqLock.
      - Probe syscalls, additions only: `syscall_aspace.cc` (the whole file), `syscall_amp.cc`,
        the `ampwindow.cc` forges, GRANT_PROBE, NEST_WITNESS.
      The rule: no `KICKOS_ENABLE_SELFTEST` in `kernel/` or `arch/`, and the selftest is an app on
      the production ABI. Fault injection moves to host suites that compile the real TU with a
      link seam; a counter's or witness's claim moves to a unit suite or an emulator-level check,
      or the counter becomes a production observable every build compiles; a probe op goes when
      a unit suite holds its claim, or its arm is rewritten on the production ABI. **Ruled
      (maintainer, 2026-10-07):** the probes that witness a silicon-only internal fact (AMP rings
      on RP2350 and ESP32-C6, the alias syncs, TLBI elision on real cores) are decided one by one
      as the audit reaches each. It runs before the arm-by-arm audit below, which then audits
      arms that already run on the production kernel.

- [ ] **M10.6.4: AUDIT THE SELFTEST FOR BLOAT (maintainer, 2026-10-03; ASSIGNED TO M10'S TAIL).** The
      selftest grows with every milestone and now needs six images on the 64 KiB STM32 parts and
      more on the ESP32's 128 KiB of IRAM. Some arms may be obsolete (a mechanism since replaced, a
      regression long covered by a host unit test), some duplicate each other, and many repeat the
      same spawn/park/check scaffolding. Audit it arm by arm: what each witnesses that no other test
      does, which ones a host unit test now covers, which share scaffolding that a helper would DRY,
      and what code each costs per image. Every deletion keeps the witness somewhere, and the
      per-region counts and skip sets follow.

- [ ] **M10.6.6: WIDEN THE ESP32'S CODE SPACE BEYOND 128 KIB OF IRAM (maintainer, 2026-10-03; ASSIGNED TO
      M10'S TAIL).** `arch/xtensa/chip/esp32/esp32.ld` links all code into the upper 128 KiB of
      internal SRAM0 (`0x4008_0000`) and all data into 192 KiB of SRAM2, executing nothing from
      flash. Two levers to verify against the ESP32 TRM (1.3.2, "Embedded Memory") and on silicon
      before relying on either: the low 64 KiB of SRAM0, the flash-cache region, which KickOS never
      enables, as further code space; and SRAM1, 128 KiB reachable as both data and instruction
      memory, which the linker map does not use and part of which the ROM loader uses during boot.
      Found when the selftest outgrew one image's IRAM.

- [x] **M10.6.2: RUN THE BENCH FLEET ACROSS BOARDS IN PARALLEL (maintainer, 2026-10-06).** `bench-fleet.sh`
      builds, flashes and captures one image at a time across every board, so a full pass takes
      the sum of every board's captures (about three hours in M10.5's final pass). Each board has
      its own probe and console, so run one worker per board and keep each board's captures in
      order: the wall time becomes the longest board's. Build every image first, so flashing never
      waits on the compiler. Boards that share a probe or a console cable (bluepill-c8 and
      blackpill on one ST-Link; picopi, pizero2350 and teensy41 on one FTDI cable) stay in one
      worker, and bench-present.sh already knows which ones those are. Each board's log tags
      stay separate, and the summary is merged at the end.
      **LANDED:** every image is built first (`BUILD_ONLY=1`), then one worker per board captures
      its images in order; boards holding a common probe or console (`board_resources`) share a
      worker; the summary and exit status are as before. `check_bench_fleet.sh` plants two
      overlapping boards, a shared cable, a build after a flash and an out-of-order capture.
      **Owed:** the remote mode, whose shipping step now holds a lock, has run against no bench
      host yet.

- [ ] **M10.6.6: BOUND THE INTERRUPT STACK ON ARMV7-M, ARMV6-M AND RX (found closing M10.5).** The trap
      depth gate roots device ISRs only where they run on a thread's or a kernel stack it sizes; on
      these three the ISRs run on the main/interrupt stack, which the roots file declares unbounded,
      so no ISR chain is measured there (M10.5 added a wake to the console TX ISR's tail).

- [x] **M10.6.2: A CONFIGURE WRITES COMPOSITION FILES INTO THE SOURCE TREE (found closing M10.5).**
      Under a parallel `-L tree` run a gate copying the tree failed once in 0.5 s and then passed:
      a configure wrote composition yaml into the source tree while the gate copied it. That gate
      (`board_refusals`, two configure refusals) went in the audit's first wave; the cause stays:
      a build must not write into its sources.
      **CLOSED:** no configure writes composition yaml; all 91 presets configured four at a time
      leave the source tree as it was. What a configure did write was `selftest_demands.py`'s
      bytecode, now run with `-B`; every gate's Python inherits `PYTHONDONTWRITEBYTECODE` from
      `gate.sh`. The M10.5 failure's own cause is not reproduced.

- [ ] **M10.6.4: THE SELFTEST APP DOES NOT BUILD WITH THE SELF-TEST OFF ON ADDRESS-SPACE BOARDS (found closing
      M10.5, older than it).** On qemu-arm64-amp2-n0 and amp3-n0 configured with
      `KICKOS_ENABLE_SELFTEST=OFF`, `selftest/main.cc` and `selftest_common.cc` call
      `kos_aspace_probe` with no guard. No CI or local configuration builds that posture, so
      `amp_prod_build` skips address-space boards; either guard those calls or stop building the
      selftest app without the self-test, then register the gate there.

- [ ] **M10.6.3: NAME THE PORT BY ITS NUMBER IN THE GENERATED PIN LISTS (maintainer, 2026-10-06).**
      `KICKOS_BOARD_RESERVED_RUNS` and `KICKOS_BOARD_KERNEL_PINS` carry each pin's port as a base
      address, so `chip_imxrt1062.cc` maps it back to a GPIO bank through a hand-written `bank_of`
      that knows banks 1 and 2 only, a second truth beside the port number the generator already
      emits for named pins. Emit the port number in `RUN` and `PIN` as well, use it in every chip
      that reads those lists, and delete `bank_of`.

- [ ] **M10.6.5: WAKE AN OWN-IMAGE AMP CONSOLE WRITER BY DOORBELL (maintainer, 2026-10-06; deferred out of
      M10.5).** A writer waiting for a peer node's console claim parks for `CONSOLE_CLAIM_POLL_NS`
      (1 ms) between offers, because a peer's release sends no wake. The proper form rings the
      waiting node's doorbell on release, so the writer parks until woken, with no poll.

## Selftest expectations still approximate (cut from M10.5)

- [ ] **M10.6.4: THREE PIECES OF THE DERIVED SKIP SETS WERE CUT FROM M10.5 AND ARE OPEN.**
      `tests/integration/gates/selftest.cmake` derives each arm's thread, capability and task-budget
      skips from the ask the arm makes (`pool_can_host`, `objects_can_host`). What it does not yet
      derive:
      - **The arena arms.** `irq_as_event` and `caller_stack` are still expected on any part with
        32 KiB of RAM or less. One free-arena figure cannot decide them: the arena never frees,
        and what an arm finds depends on the order in which earlier arms seat thread stacks and
        reserve blocks. Measured on microbit's fourth image: the arena is exhausted by
        `caller_stack`, and five earlier arena arms still run, although seating every free slot
        would leave 0 bytes. The proposed design:
        - a starvable arm seats every free thread slot before it reserves;
        - its block size and alignment go into the image as a symbol the compiler computes;
        - the composition states its thread count and the cursor once the pool is seated in the
          image;
        - the expectation is derived after the link from those symbols, and the manifest rows
          become post-link;
        - `bench.sh` reads its row after the build, which is owed on silicon.
      - **The create-then-skip sites.** About 200 arms still create an object or spawn first and
        report a "pool too small"-class skip when refused. None fires on any fleet posture, so no
        expectation reads them. The proposal: convert them to ask first through one helper, and
        add a static gate that refuses such a skip reason not preceded by its ask.
      - **The IRQ handle budget.** `KICKOS_TASK_IRQ_HANDLE_BUDGET` is not modelled as a pool. The
        IRQ arms that claim a line before asking for their objects make the claim part of the ask
        instead.

## The toolchain release (maintainer, 2026-10-04)

- [ ] **THE TOOLCHAIN PROBABLY SHOULD NOT REACH A USER AS A CONAN PACKAGE (maintainer,
      2026-10-04).** To use a prebuilt toolchain today a user needs Conan:
      `tools/kickos-toolchain.sh` runs `conan profile detect` and `conan cache restore` on the
      release archive, and the build finds the compiler through the Conan cache. That is heavy for
      someone who only wants to compile for one board. To settle: a plain archive a user unpacks
      and points `KICKOS_TOOLCHAIN` at, with Conan kept for building the toolchain from source and
      in CI if it still earns its place there.

## The console collision class closes at the EMITTER, and the gate side has run out of room

- [ ] **ELEVEN OF THE TWELVE DAMAGING COLLISIONS LAND ON A VALUE, WHERE NO GATE-SIDE TOLERANCE
      CAN EVER BE SOUND.** A bounded in-order match cannot separate a longer value from a broken
      record: an intact `ADDR=0x403780009` satisfies a match for `ADDR=0x40378000` at span 15
      with nothing interleaved between the characters, so the tolerance that recovers a shredded
      record also accepts a wrong one. Presence can be matched loosely; a `NAME=VALUE` cannot.
      Gate-side work has taken the residual to 0.76 percent and every remaining case refuses
      rather than accepts, which is the safe direction and is also the end of that road. **The
      remaining 0.76 percent is what an emitter-side fix retires**, and nothing on the gate side
      retires it.
      **The shape of the remedy**: `arch_console_write` in
      `arch/arm64/chip/virt_arm64/chip_virt_arm64.cc` is an unlocked byte loop, so two kernels on
      one PL011 interleave at byte granularity by construction. Give that path a per-line claim
      against the peer, held across the whole record rather than across one writer call, so a
      record reaches the wire whole and a gate reads a value it can trust.
      **`arch/arm/chip/rp2350/chip_rp2350.cc` is the working precedent and the design is already
      settled there**: the claim is held to the LINE and not to the chunk, because
      `console_write_user` calls the writer once per 64-byte piece and a claim released per chunk
      lets a peer's whole line land inside one of this node's; ownership is bounded at both ends,
      by a deadline taken when the claim is taken and by a peer that spins its budget out against
      an expired claim releasing it under the holder, so the bound holds with the holder gone; the
      release is conditional on the published owner still naming this node, so a stolen-from node
      does not end the thief's claim; and a caller that LOSES the claim writes anyway, the cost
      being one shredded line. That last property is what keeps it inside
      `docs/reference/console.md`'s "always works, even while dying" guarantee, and it is the
      property any port of this must keep.
      **It is a lock on the console path INCLUDING the fault path, so it is a cost decision and
      not a bug fix.** Every panic, every fault dump and every dying write pays the claim, on a
      path whose whole contract is that it works when the system does not. The RP2350 body prices
      that with a spin budget, a 50 ms hold bound and no interrupt masking (`console.cc` forbids
      holding the chip transport under `IrqLock` across a transmission), and a caller that cannot
      get the claim in budget still writes. Whether `qemu-arm64` should pay the same on every
      record is the decision this item is asking for, not something to land quietly.
      **Not implemented on purpose.** Recorded with its precedent so the decision is taken on the
      cost, and so the next reader does not spend another pass looking for a gate-side answer that
      does not exist.

## The file:line:column indirect-call binding is fragile, and replacing it is its own piece

`tests/static/trap_redzone_indirect.txt` binds every indirect call site by
`<basename>:<line>:<column>`, so any edit ABOVE a bound site invalidates it. Measured during
M7.10: the `irq.cc` bindings were re-taken three times in one day, shifting 30, then 74, then 75
lines, and the 630 `console_tx.cc` sites shifted by one when a single `#include` landed above
them. No bound call was touched by any of those edits.

**Not a cleanup.** A stabler key (an enclosing symbol plus an ordinal, for instance) has to keep
the property the file exists for: an indirect site reachable from a root and NOT declared is a
HARD FAILURE, and a declaration naming an absent callee is one too. A scheme that cannot
distinguish "unbound site" from "key no longer resolves" converts that hard failure into a
silent skip, which is worse than the churn it removes. Two gates read the file
(`trap_redzone.py`, `console_reach.py`), so it is a change to how a class of gates binds
evidence.

Deliberately left for its own milestone.

## f302nucleo-st runs two threads to pay for the priority ceiling (parked, milestone unassigned)

- [ ] **THE BOARD LOST A THREAD SLOT AND THE CHASE IS PARKED, NOT ABANDONED.** The scheduling
      grant's priority ceiling is unconditional by design -- it closes a starvation hole on
      every board, single-core included -- and costs `Task` four bytes through alignment. On
      `f302nucleo-st`, which had no arena slack, that tipped the link-time assert refusing a
      board that advertises more thread slots than it can seat. Measured rather than guessed:
      at three threads the link fails at user stacks of 1024, 896 and 768 alike, so the user
      stack is NOT the lever; two threads at 1024 links.
      **THE DOMINANT TERM IS NOT ESTABLISHED, AND THE KERNEL STACK IS NOT IT.** `CHIP_STM32F302`
      selects no `HAS_MPU` and armv7m does not make the blocks mandatory, so
      `KICKOS_KERNEL_STACKS` resolves 0 here and this board carves no per-slot kernel stack at
      all. The arena assert's remedy clause names the blocks conditionally, and reading that
      clause as a diagnosis is what produced the earlier attribution. The same resolution voids
      the `KICKOS_KERNEL_STACK_SIZE` probe rather than leaving it open: that knob reaches
      nothing on this board, so an inert probe was the answer and not a tooling failure. What
      the board spends its arena on instead is open.
      **The cost of the drop**: this board's selftest skip set grows by whatever needs a third
      thread, and the size of that set is unwitnessed -- `f302nucleo` is main-bench only and
      has no emulator, so only a silicon run names it. Any record saying this board runs three
      threads is now wrong.
      Chase it with the M8 footprint work, where the question is the right shape.
      **That pointer is now stale.** `roadmap.md`'s M8 section, cut into M8.1 through M8.11, carries
      no footprint sub-milestone; nothing there owns a fleet-wide footprint chase. This item's home
      is unassigned until one is.

## M7.7 -- AMP

S7 is landed: `qemu-arm64` ships an AMP posture as a preset, the instance index comes from the
core identity, and the shared window's validation is driven by forged publications rather than
argued. What is left here is what the step deliberately did not close, and one ruling it hands
back.

- [ ] **`KOS_ASPACE_OP_CAP_SEED_VA` DOES NOT KEEP ITS OWN PROMISE.** Its contract says the
      address it hands back is one nothing in the caller's space names, and it cannot check
      that: the CHILD whose stack collides does not exist when the address is chosen, so no
      list it could consult describes that space yet. `VR_USTACK` closed the other half, the
      caller's own stack now being in its range list. It is scaffolding, so this is a note
      rather than a defect, but an arm built on that promise is resting on a layout.
- [ ] **THE PER-NODE COUNTERS ARE RELAXED ATOMICS AND THE ROWS ARE STILL SHARED RAM.** One
      writer per row makes the load/store pair enough for a torn read, which is all that was
      claimed. It is not protection: a peer kernel writes any row it likes, and the labelling
      below is the whole of the answer until partitioning lands.
- [ ] **AMP IS PROTOCOL SCAFFOLDING AND NOT A HOSTILE-KERNEL BOUNDARY, and the window's header
      says so.** The mint sits outside the shared window, but every node maps the same writable
      kernel RAM, so a compromised peer kernel rewrites validation state and every other node's
      kernel data. The label stands until per-node memory partitioning lands, which is the
      partition layout the contract leaves deliberately open.
- [ ] **THE WINDOW IS NOT REACHED THROUGH AN ENDPOINT YET, and that is the freeze's remaining
      half rather than a second API.** One IPC mechanism with locality resolved below the seam
      is the freeze; no user-facing cross-node call was added, so nothing above the syscall can
      tell the two localities apart. What a later step wires is the resolve-to-ring branch, and
      the reply path is the hard half: it is built on a raw local thread pointer with a
      one-shot generation guard riding the minted capability, and a remote caller has no such
      thread in this kernel. Priority donation across nodes has no meaning at all.
- [ ] **A PEER NODE RUNS NO SCHEDULER, and giving it one is the AMP partition layout the
      contract leaves open.** The arena is one linker region with a link-time assert modelling
      its allocation order; a per-node arena, and whether instance-keyed storage can be made
      separately protectable, are the same question.
- [ ] **THE PORT MINT IS ONE MASK SEATED FOR EVERY NODE FROM ONE KERNEL INIT.** That is the
      static configuration act the freeze names, and it is deliberately not an answer to who
      may mint one: the mask is written once before any node can be poked and read-only after,
      and it sits OUTSIDE the window so a far side able to write it cannot validate itself.
- [ ] **THE NODE BUILT HERE IS TRANSLATING-MODEL AMP, AND THE FIRST REAL AMP SILICON IS
      MIXED-MODEL.** Both sides of this posture have an MMU. The CV1800B does not: its main C906
      has one and its companion C906 has none, in the datasheet's own words, which is also why
      that part fails the predicate's symmetry requirement and is an AMP part BY THE PREDICATE
      rather than by judgement. Nothing in the tree witnesses a translating node beside a region
      node, and that pairing is a different port from the one just written.
- [ ] **`mhartid` IS HARDWIRED TO ZERO IN THE OPENC906 RELEASE**, the integrator customising it
      per instance, and no document on hand states what the two CV1800B cores report. An AMP node
      that takes its instance index from its core identity, which is the contract's own rule and
      what this step implemented, therefore has no register to stand on there. What those two
      cores report has to be established before an AMP posture is claimed for that part.
## M7.6 -- the i.MX 8M Plus EVK and the per-part predicate

S6b is landed: the predicate splits by owner, and a second armv8a part boots under its own GIC-500.
What is left here is what the step deliberately did not close.

- [ ] **THE SECONDARY RELEASE IS UNWRITTEN, AND THIS IS THE ITEM TO ARGUE WITH FIRST.** The A53
      cluster meets all six properties and declares the part's three; what has no vehicle is
      STARTING a core. QEMU's `imx8mp-evk` carries no PSCI conduit, holds cores 1 to 3 powered off
      with nothing able to clear it, and models the reset controller and the mux registers as
      unimplemented devices. The silicon sequence is recorded in `docs/reference/boards.md` down to
      the register offsets, including that the manual states no ordering for it and that a released
      core arrives at EL3 owing the same handover the primary gets. Writing it would mean an
      entry-point pair split across two registers, on a path no preset can compile or run. Held
      back on that ground rather than on cost; reverse it if a vehicle appears or the silicon does.
- [ ] **THE SYSTEM COUNTER'S OWN ENABLE IS NEVER WRITTEN.** The chip programs `CNTFRQ_EL0` at EL3,
      standing in for firmware, but not the counter block's control register: that block is
      unmodelled here, so a write to it could not be witnessed either way. On silicon out of reset
      a stopped counter reads a constant with a plausible frequency beside it, which is the shape
      that hangs a bounded wait instead of reddening it, so this is a real silicon hazard rather
      than tidiness.
- [ ] **THE CLOCK AND RESET GATES ARE ABSENT FROM THE RESERVED SET.** `arch_reserved_blocks` names
      the GIC alone, while this die's clock controller and reset controller reach every peripheral
      on it, so a domain handed either could stop a core it does not own. Nothing in the tree grants
      an MMIO window on this board yet, so the gap has no reachable consequence and no arm could
      witness a refusal; it becomes real with the first grant.
- [ ] **NO BAUD RATE IS PROGRAMMED ON THE CONSOLE.** The divider runs off a clock this port
      configures nothing else in, and the model ignores it entirely. Real hardware needs it, and it
      arrives with the clock tree rather than on its own.
- [ ] **THE EL1-ENTRY PATH AND THE EL2 REFUSAL HAVE NO VEHICLE.** The machine always hands over at
      EL3, so the branch that accepts a firmware handover at EL1 is never taken and the EL2 refusal
      beside it has never fired. Both are one branch each and were written rather than left out, an
      unnamed handover being worse than a named refusal.
- [ ] **THE BOARD IS IN NO CI JOB.** It is a local `ctest` preset only, like every arm64 preset but
      the base one. Adding a job is cheap, the aarch64 toolchain action already existing; not done
      here because no arm64 variant preset is in CI and doing one and not the others would be
      arbitrary.
- [ ] **THE TWO arm64 CHIP PORTS ARE NEAR-CLONES. RAISED BY AN EXTERNAL AUDIT OF THIS BRANCH AND
      DELIBERATELY NOT ACTED ON HERE.** `virt_arm64` and `imx8mp` duplicate the linker script, the
      MMU startup, the generic-timer body, the semihosted shutdown and the high-half helpers. The
      boundary the audit proposes: extract the common structure with the chip addresses and the
      archive names parameterised, keeping the EL3 handover and the UART wiring chip-specific. Not
      done on this branch because it is a refactor across two SHIPPED ports, which is milestone-
      sized, and this branch is meant to merge. The evidence for it is that the audit's own timer
      finding had to be fixed in both files, as did the boot stack ordering beside it, and the
      comment trim before them.
- [ ] **(superseded) A ONE-CORE-KERNEL GICv3 PRESET, OWED TO THE AMP STEP AND NOT TO THIS ONE.** Nothing
      witnesses the `KICKOS_NUM_CORES > 1` folds in `arch/arm64/common/arch_arm64_gicv3.cc`, and
      the one-core fold check in `arch.h` cannot: it does not apply above one kernel core, and the only preset
      carrying that backend runs four. Deliberately NOT added now -- an arm nothing exercises says
      nothing, and the configuration where one kernel core meets a live GICv3 is
      `KICKOS_MULTICORE_AMP`, which sets one kernel core while the image still drives four. That is
      where the preset earns its fleet time. Built and booted by hand once during S6, the full
      selftest passing at one core under GICv3, so this is an unwitnessed arm rather than an
      unbuilt one.
- [ ] **`ICC_SGI1R_EL1.RS` IS IMPLEMENTED AND UNEXERCISED.** Every part in scope has affinity 0 in
      0 to 3, so the range selector is always zero and the send emits one write per cluster. A core
      with affinity 0 at or above 16 on an interface whose `ICC_CTLR_EL1.RSS` is clear REFUSES at
      bring-up rather than raising an interrupt that goes nowhere, and that refusal rests on
      `arch_cpu_id`'s existing claim of one cluster of symmetric cores. Nothing on this bench can
      produce the case.
- [ ] **NO ARM OBSERVES AN INTERRUPT GROUP.** The group mismatch is caught only because the timer
      PPI shares the group decision with the doorbell SGI, so a backend that grouped them
      separately would boot, answer doorbells and hang with every gate green. Making a group
      readable by an arm is a decision, not a fix.

## M6.5

Steps C0 through C3 are landed. What is left here is carried-in work and what the steps deliberately
did not answer.

- [ ] **Neither new capability kind has a user-facing MINT.** A frame-run and an address-space
      capability exist only through `KOS_ASPACE_OP_CAP_SEED` and `KOS_ASPACE_OP_CAP_SELF_SPACE`,
      which are selftest scaffolding. C1 through C3 witness the objects, the map pair and the
      sharing; WHO MAY MINT ONE is undecided, and that is the question a real mint has to answer
      rather than a gap in the steps. It wants a ruling before an ABI freeze, not before the audit.
- [ ] **A MINT MUST NOT LET TWO LIVE RUN OBJECTS SHARE A PHYSICAL BASE, AND THE REASON IS
      OWNERSHIP RATHER THAN IDENTITY NOW.** The identity half is retired: `aspace_cap_unmap` no
      longer matches a mapping to its run by the first page's physical address read back with
      `arch_aspace_frame_at`, the range carrying the run's SLOT instead, so the map editor makes
      no uniqueness assumption at all. What survives is the lifetime: each run object carries its
      own holder count and the last drop hands the frames back to the pool, so two objects over
      one base return the same frames twice -- the first to empty frees them under the second's
      live mappings, and nothing notices until the pool refuses the second free. A user-facing
      mint has to answer that; it is a constraint on the mint and not an assumption the map
      editor may make.
- [ ] **The type field is exactly full: a third kind is a repartition.** Values 6 and 7 are spent,
      and `KCAP_TYPE_BITS` is welded to the reply call sequence packed beside it
      (`KCAP_REPLY_SEQ_LO_BITS`), so widening costs that and breaks the frozen 8-byte `CapEntry`
      and `tests/unit/capreply/capreply_packing.cc`. `CAP_KIND_MAX` refuses it at compile time now,
      which is the point; the decision is what a fourth kind would be worth.
- [ ] **`tests/static/trap_redzone_indirect.txt` binds call sites by exact `file:line:column`.**
      Every comment edit, every inserted line and every reflow in a bound file shifts the
      bindings, so an unrelated change breaks the gate and the fix is a hand edit of a table.
      Two files had to be reverted from a comment sweep for this alone. Bind on something the
      edit does not move: the enclosing symbol plus the callee, or a marker in the source.
      Re-derived by hand-driven tooling twice now. Two approaches that do NOT work, so the
      next attempt does not spend them again: `#line` directives to restore numbering, which
      never resync for active code after one lands inside an `#if`-disabled region; and a
      per-instruction "only the immediate changed" check, since on Thumb the line number's
      VALUE selects its encoding and so moves widths, branch offsets and instruction counts.
      What does work is a padded control, every comment hunk restored to its original length
      so each code line keeps its physical number.

## Retired from the M4.7.2 review backlog (triaged 2026-08-06)

M4.7.2, .3, .5 and .6 closed most of the review backlog without the entries being updated. What
survives is below; everything else was re-verified fixed against tree `82fa51f`.

- [ ] **`handle_close` does not refuse a RESERVED index, and one such call costs a thread its
      console for good.** `cap_lookup` bounds on `thread_cap_capacity` and nothing else
      (`kernel/syscall/cap.cc:485`), so `handle_close(c, 0)` resolves -- slot 0 is seated, its
      cap-gen is 0, and the bare handle 0 gen-matches -- and the close bumps that gen
      (`cap.cc:786`). Userspace names stdout as the bare constant `KOS_CAP_STDOUT`
      (`system/include/kickos/sys/cap_index.h:39`) and `cap_seat_stdout` re-seats the slot without
      resetting the gen (`cap.cc:882-904`), so no later publish makes handle 0 resolve in that
      thread again. LATENT: nothing in `user/`, `system/`, `tests/` or `examples/` closes a
      reserved index. Fix is a refusal below `KICKOS_CAP_FIRST_DYNAMIC` plus a selftest arm
      proving it. Also stated in `docs/design-capability-table.md` section 11.
- [ ] **`KICKOS_CAP_RUN_OFF_POOL` reserves one run more than the true peak.** It is 1
      (`cmake/cap_geometry.cmake:26`), the in-flight spawn run, where the peak of concurrently
      ATTACHED runs is `KICKOS_THREAD_SLOTS`: `ThreadPool::alloc` returns the reclaimed slot's run
      before the spawn's `cap_slab_attach` (`kernel/syscall/syscall_thread.cc`), and the slot a
      spawn targets holds none either way, so the in-flight run REPLACES a pool one rather than
      adding to it. Cutting to 0 saves one child-width run of `.bss` and must move
      `cmake/cap_table.cmake`'s footprint arithmetic with it. Deliberately not folded in: it
      spends the last margin on an allocation whose exhaustion is indistinguishable from a full
      thread pool, both `-KOS_ENOMEM`, so it wants its own measurement. M4.7.7 moved the number
      from 2 to 1 by seating root in the pool, which did not touch this margin: the total
      `KCAP_RUN_COUNT` is unchanged.
- [ ] **`grant_reserved` has three `tap::partial` exits the bench cannot tell apart.**
      `user/apps/common/selftest/main.cc:2014` partials at `:2037` (granule alloc failed), `:2064`
      (board reserves nothing) and `:2190` (board mints no DEV window), while
      `selftest/CMakeLists.txt:116` matches `KICKOS_EXPECT_PARTIALS` on the test NAME alone, so a
      partial from an unexpected cause reads as the expected one.
- [ ] **`docs/reference/architecture.md` has never had a full-document correctness audit.** The
      M4.7.x edits corrected only the rows a grep surfaced; the rest is unreviewed. It is the one
      reference doc no reviewer covered in full -- the agent assigned to it died without
      reporting -- and the 2026-07-29 codebase sweep that used to back it up has been deleted
      from the tree, so nothing banked covers this file at all.

## Found during the M4.7.5 configuration-mechanism work (triaged 2026-08-06)

The whole fleet is on Kconfig now, so anything that once read "scoped to a crossed board" applies
to all 20.

- [ ] **Nothing checks that the generated fragment and the `-D` translation carry the same knob
      set.** `tools/kconfig/genconfig.py:35-70` owns 23 fragment variables (8 string, 10 int, 5
      bool); `CMakeLists.txt:92-146` translates a bare `-D` into a `CONFIG_*` request over its own
      lists; nothing compares the two. A knob in the fragment but not the translation is one the
      fragment SILENTLY OVERWRITES -- that shape has now bitten three times (the posture, the five
      booleans, and the service-list selection knob/the pin-map selection knob, whose omission reddened four sim
      gates). `tools/kconfig/test_genconfig.py` drives `tools/kconfig/genconfig.py` DIRECTLY, so
      the CMake translation never executes under any gate, and no case round-trips the service-list
      or the pin-map selection knob, a provisioning integer accepted as an override, or a boolean
      forced to `n` against a defconfig that sets it `y`. The only real exercise of the
      translation is an explicit service-list selection in the four sim gates.
- [ ] **Five booleans reach C from CMake, not from the generated header.** `KICKOS_DEBUG`,
      `KICKOS_ENABLE_SELFTEST`, `KICKOS_BENCH`, `KICKOS_SHUTDOWN_TO_BOOTLOADER`
      (`CMakeLists.txt:245,253,278,295`) and `KICKOS_SCHED_PERIODIC_TICK`
      (`kernel/CMakeLists.txt:93`) arrive by `add_compile_definitions`, because
      `tools/kconfig/genconfig.py:30-31` emits only `INT`/`HEX` symbols. The fragment is therefore
      load-bearing for them and `option()` must defer via CMP0077. Converting the emitter to
      `#if`-style booleans retires the fragment lines and the deference together.
- [ ] **A board's provisioning is repeated once per variant and nothing compares the copies.** The
      corpus is `git ls-files '*/configs/*/defconfig'`, 52 defconfigs over 20 boards at the time of
      writing (20 `base`, 14 `flat`, 13 `st`, 3 `bench`, 2 `telem`), each a
      COMPLETE statement rather than a delta on `base` -- which is the Kconfig model and what
      `savedefconfig` writes back. A board with `base`, `st` and `flat` states
      `KICKOS_MAX_THREADS` three times and an edit to one is silent in the other two.
      `tools/kconfig/test_genconfig.py` resolves every one of them and asserts only that; it never
      diffs a variant against its base, and `savedefconfig` regenerates one variant from the live
      `.config`, so it cannot catch a divergence either. Cheap first cut: a gate asserting every
      variant agrees with its board's `base` outside a per-variant allowlist of the symbols that
      variant exists to change. The rule behind it is the open question -- nothing declares which
      axis a variant owns. Expressing a variant as `base` plus a fragment was considered and
      rejected: it breaks the `savedefconfig` round trip.
- [ ] **The "is this a knob?" rule has never been applied once over the whole symbol set.** The
      rule is in the design -- a hardware fact earns a Kconfig declaration only if some option's
      availability or default depends on it -- but it was only ever applied to the two symbols the
      maintainer asked about (`KICKOS_MAX_IRQ`, `KICKOS_RX_INTB_ENTRIES`), both of which left
      Kconfig for `chip_limits.h`. `KICKOS_CONSOLE` is the named open case: an unconditional
      prompted `choice` at `Kconfig:183-202`, a real choice on a board with two transports and a
      fact on a board with one, with nothing distinguishing them.
- [ ] **A wrong `arch_mpu_region_pow2` / `arch_mpu_min_region` literal is caught by nothing
      in-tree.** `cmake/boot_arena.cmake:53` (`_kickos_seam_int_in_file`, driven from `:140-141`)
      regex-scrapes the return literal out of the same backend TU the link resolves, and also
      reimplements the linker's archive-member selection rule. It is the one value the ownership
      rule cannot place: there is no configuration behind it, so nothing resolves it and there is
      nowhere for it to come FROM. `rx72m` silicon is the only check for the RX MPU. Already
      recorded in `STATE.md`; filed here so it survives the next STATE.md rewrite. Cited without
      a line number ON PURPOSE: it has already moved once, which is the whole reason for the copy.
- [ ] **Decide whether the per-app build stamp should be reproducible.** It folds `__DATE__` and
      `__TIME__` in an app TU (`user/include/kickos/app.h`), so its CODE size varied between two
      builds of an identical tree (measured at 0x8c, 0x90 and 0x94) and every later address shifted
      with it. R2.2 turned the app's half into DATA (`kickos_app_build_raw`) and moved the
      reformatting into `kmain`; the flash-fix pass turned that half into a `const`
      `kickos_app_build_time` and deleted the reformat, so the app's half is now a `.rodata`
      string and no code at all, and the code-size variation is gone. The timestamp is not. It
      exists to answer "did the APP change or was the image relinked", which a content hash would
      answer without perturbing code size. While it stands, **"byte-identical
      image" is not a claim this tree can make** -- and `docs/reference/boards.md:2199` makes it,
      correct in intent ("Tree identity is the test, not hash identity") but wrong in wording.
- [ ] **The C surface is gated on the INSTALLED package only, not per board.**
      `tests/static/check_public_headers.sh` now carries a C11 arm beside the c++17 one. Its
      corpus is DERIVED, not listed: a header whose code guards `extern "C"` with
      `__cplusplus`, plus that header's include closure (24 of 63 headers on a sim package).
      No exclusion list to rot -- `sys/bytes.h`, `sys/spi_service.h` and `kos.h` carry no
      guard at all, so they never claimed C and the rule drops them by itself. `arch/include/kickos/arch/arch.h`
      now states C++-only with an UNGUARDED `extern "C"`, which also takes the per-arch
      `context.h` (`alignas`) out of the corpus with it.
      What is left: the one caller (`check_oot_export.sh`) installs ONE board's package and
      uses the host `gcc`, so the per-arch `context.h` of the other five arches and every chip
      header are unchecked as C. Its `--tree` form walks the SOURCE tree per board with that
      board's own cross compiler; the two forms are complementary.
- [ ] **`abi.h`'s p-state enum is C++11/C23, not C11.** `user/include/kickos/sys/abi.h`
      declares `typedef enum kos_pstate_e : uint32_t`, and a fixed underlying type is
      something C did not adopt until C23. GCC accepts it as an extension and says nothing
      without `-pedantic`, which `KICKOS_WARN_FLAGS` does not set, so it is invisible to both
      of the things that now compile C: `hello_c` (the tree's only `.c`) and the C11 arm
      above. It breaks the day anyone adds `-pedantic` to a C app, or a stricter C compiler
      reads the header. Recorded, not fixed, on purpose: nothing is broken today. The fixed
      32-bit width IS the stable ABI here, so the fix keeps it -- an unnamed `enum` for the
      three values plus a plain `typedef uint32_t kos_pstate_t`, not a narrower enum.

## Found auditing the panic and fault scope (2026-08-06)

Two audits are in flight on this, both gitignored spikes in the main checkout
(`.session/spikes/audit-panic-scope-syscall.md`, `.session/spikes/audit-panic-scope-faults.md`).
The items below are the parts that belong in tracked history whatever those audits conclude.

- [ ] **A terminating image without `KOS_AUTH_SYSTEM` cannot exit cleanly: a successful termination
      reports as a crash.** Both routes panic when the shutdown is refused. A returning `main`
      reaches `kos_panic("root: shutdown refused")` in `root_entry` (`kernel/init/kmain.cc`), and
      M4.7.7's root-exit path reaches `kpanic("root: exit shutdown refused")` in the `KOS_SYS_EXIT`
      arm (`kernel/syscall/syscall.cc`); both end in `kfault_terminate`, which exits with the fault
      status 132. `system/include/kickos/sys/init.h` already requires the authority for a returning
      `main` and `docs/reference/invariants.md`'s `init-return-is-shutdown` repeats it, so the rule
      is documented and still only discoverable at runtime; the single gate is
      `user/apps/common/rootauth`, which passes the panic text to `tests/integration/check_app_arms.sh` as a
      must-NOT-appear marker. Proposed shape: refuse the combination at CONFIGURE time, where the
      tree already prefers failing loud and early (the HAS_MPU-without-`mpu.cmake` refusal in the
      root `CMakeLists.txt` is the model). The obstacle to price first is that nothing declares "this
      image terminates" and the authority mask is a C symbol in the app's own translation unit
      (the app authority macro) that no build file reads, so the gate needs a declaration that does
      not exist yet.
- [ ] **The fault path is thread-scoped on FIVE backends (armv6m, armv7m, rxv3, rv32imac, sim)
      and still system-terminal on ONE (xtensa); on that one, isolation still buys only
      detection, attribution and prevention of cross-domain corruption, and NOT availability.**
      `arch/arm/armv7m/arch_armv7m.cc`'s `kickos_armv7m_fault_report` now opens with
      `if (kickos_fault_kill_thread(frame)) { return; }`, which redirects the stacked PC to
      `kickos_thread_fault_exit`; `kfault_terminate()` is the FALLBACK, not every path.
      `kickos_fault_kill_thread` exists on armv6m, armv7m, rv32imac and the sim (plus its shared
      body in `kernel/init/fault.cc`), gated by `user/apps/common/faultsurvive` and
      `user/apps/common/drvdeath`. armv8-m boards run the armv7m reporter (`KICKOS_ARCH` is
      `armv7m` on `qemu-m33` and `pizero2350`) so they inherit the same early return. xtensa and
      rxv3 have no such function: xtensa's `kickos_lx6_fault_report` and rxv3's fault handler both
      still reach `kfault_terminate()` on every path (rxv3 after `kickos_isr_fault` names the
      task), so the original claim stands for those two only. **Correction to carry: `EXC_RETURN`
      bit 2 does NOT distinguish a user-context fault from one taken mid-syscall.** It selects MSP
      against PSP, and a syscall runs in privileged thread mode on the calling thread's own PSP,
      because `SVC_Handler` rewrites the stacked PC to `svc_trampoline` and clears
      `CONTROL.nPRIV`; the reporter reads that bit only to print the stack's name. The sound
      discriminators already in the tree are the stacked `xPSR` IPSR field (printed but never
      decoded), a `CONTROL.nPRIV` read in the handler, and `ctx.resting_npriv`. What a
      thread-scoped death already leans on: `cap_teardown` plus `sched::exit_current` tear down a
      dying thread's capabilities, its domain reference, its held mutexes, its served endpoints
      (EPIPE-waking every parked sender) and its owned IRQ lines. Two real gaps still open: there
      is no grant capability type, so spawn-time windows go only transitively with
      `domain_release`, and a `kos_mem_self_grant` window in `Thread::regions` is never cleared at
      exit at all; the stack, the capability slab run and the orphaning of children are all
      deferred to `ThreadPool::alloc`'s reclaim sweep. `kernel/include/kickos/domain.h` already
      documents a supervisor respawning after a death, requiring that a supervisor learning of it
      by watchdog, timeout or a future join must join before respawning, and no fault path can
      reach that today.
- [ ] **Standard C `exit()` does not link on the ARM ports.** It pulls `__libc_fini_array`, whose
      `_fini` no linker script in this tree defines: every `.ld` carves a `.kickos_app_fini_array`
      output section and then `ASSERT`s it empty, and not one defines or provides `_fini`. The
      failure is "dangerous relocation: unsupported relocation", measured on mps2-an386 (preset
      `qemu`) and banked nowhere; the mechanism is recorded in exactly one place,
      `user/apps/common/sched_exit/main.cc`. The live routes into `_exit` are therefore `abort()` and
      a failed `assert`, both newlib's own, reaching this tree's `_exit` in
      `user/src/newlib_stubs.cc` (force-linked fleet-wide through `-Wl,-u,_exit`). Against this
      project's consumer-API surface a plain C `main` should be able to call `exit()`, so the item is
      to satisfy `__libc_fini_array` in the linker scripts rather than to leave the standard call a
      link error.

## Found by the M4.7.9 ten-angle review (2026-08-07)

Both items were measured on `23b9abb` and neither is a defect this milestone introduced. Both are
scheduler and console core-path work, so neither rides M4.7.9. The capture behind both is
archived `M4.7.9_teardown_latency_meas.md`.

- [ ] **STILL OWED by the fix above, and it is silicon: narrowing the guard puts MORE traffic
      through the preemptible window between a fault redirect and its stub**, so the fault-record
      race gets more likely, not less. `fault-record-is-printed-only-by-its-owner` is the invariant
      that carries the weight now, and no host gate can discharge it. Wants an enforcing board with
      a fault arm under teardown pressure.

- [ ] **The M4.7.9 fault-path console-pressure figures are stale and want a re-take.** Measured
      when `console_tx_write`'s overflow branch still drained a full ring under `IrqLock`: the
      preemptible THREAD FAULT dump grew from 105us to 290us (xmc4800-relax) and from 132us to
      876us (frdmk64f) under synthetic console pressure. That branch no longer bit-bangs under the
      lock -- `console_tx_write` queues in ring-sized chunks and waits UNMASKED between them, so the
      masked window is one ring copy (at most 128 bytes on every CRLF board), independent of baud,
      at a sub-1% duty cycle, with a bounded synchronous burst only when nothing drains at all.
      `tests/unit/consoleown/masked_window.cc` scores the old code at 4096 masked pushes and the
      new at 1. Re-take the two figures under the same synthetic pressure. The drop-on-overflow the old comment
      rejected is moot: nothing is dropped and nothing stalls.

- [ ] **`console_tx_flush_sync` still drains a full ring under `IrqLock`** (up to about 44ms).
      Unreachable from an unprivileged thread -- panic and deinit only -- so it is not the DoS the
      producer path was, but it is now the largest masked window left in the tree.

## Left out of the M4.7.9 diagnostic catalogue, on purpose (2026-08-07)

Both were considered while `include/kickos/diag.h` was built and both were declined for a reason,
not deferred for lack of time. Recorded so the next reader does not re-derive the reasoning and
"finish the job".

- [ ] **Driver bring-up prose is NOT in the catalogue, and a second table would be a different
      mechanism wearing the same name.** `system/` carries 76 `kos::print`, 5 `emit`, 7 `win_puts`
      and 7 `wire_puts` sites, worth 755 bytes on xmc4800-relax hello_c per
      archived `M4.7.9_footprint_meas.md`. It earns **zero** on both boards that select the
      short column: neither bluepill-c8 nor f302nucleo links a driver archive at all. It also goes
      through the userspace `kos::print` rather than `kputs`, and lives in `kickos_system` rather
      than the kernel, so it cannot share the kernel catalogue's include or its emit path.
      Becomes worth doing only if a driver-carrying board ever selects `KICKOS_DIAG_TERSE`; until
      then the saving is hypothetical and the second table is pure surface.

- [ ] **`KICKOS_DEBUG_ASSERT` still stringifies its whole condition** (`kernel/include/kickos/debug.h`,
      9 sites), so it is the one diagnostic the short column does not reach. Left alone because
      `KICKOS_DEBUG` is `n` on every board and in every preset: wiring a short arm now would add an
      `#if` branch that NOTHING compiles, which is the exact shape that let the bounded-waits knob's
      off posture rot unbuilt until M4.7.9 deleted it (that symbol is not spelled here, because it
      no longer exists in the tree). If `KICKOS_DEBUG` ever becomes selectable by a
      preset, this needs the same treatment `KICKOS_ASSERT` got: drop the condition text and pass
      `__FILE_NAME__` and `__LINE__` as SEPARATE arguments (measured 4x better than one joined
      `"file:line"` literal, see the capture).

## M4.7.4 -- delete the legacy management (nothing is released before the ABI freeze)

KickOS is unreleased and will not ship before the ABI-freeze milestone, so
**there is no backward compatibility to manage**. Anything that exists only because something else
USED to exist is cost: it must be kept in sync, it reads as a supported path, and it makes a deleted
thing look alive. `roadmap.md`'s ledger
carries the number and the class definition.

**OWED AT THE FREEZE ITSELF, and nothing else in the tree records it: a full doc and comment
sync pass.** The instruments that keep prose honest are deliberately partial and stay that way.
`check_doc_names.sh` validates a PATH and an UPPERCASE-prefixed IDENTIFIER, so every lowercase
seam and function name a design page cites is outside its corpus -- which is how
`kickos_rx_dev_pending_line` stayed valid in `docs/design-driver-era-scope.md` long after it
stopped existing anywhere in the tree. Widening it was considered and REFUSED: it is a helper for
keeping docs roughly in step, not a proof, and a checker that cries wolf gets disabled. The
answer is a deliberate pass at the freeze, when the ABI stops moving and prose written against a
moving target can be reconciled once against a fixed one, rather than a gate that tries to hold
the line continuously against a target that is still moving.

**The sweep has been RUN.** M4.7.3 audited all four surfaces and banked the inventory at
`.session/spikes/legacy-audit.md` (gitignored, main checkout only). M4.7.4 is execution against the
rows below, not discovery. Every row was re-verified at `6f2eb55`.

**Class 1 (tombstones) and class 2 (fallbacks reachable only from a broken build) are EMPTY.** The
one instance of each was `KICKOS_MAX_HANDLES`, and both died with M4.7.3's generated header; the
supply assert that commit orphaned was deleted in the same milestone. An enumeration of every
`if(DEFINED)` (11), `message(FATAL_ERROR)` (50) and `#error`/`#warning` (17) site found no other
instance -- each names something that still exists.

**`.github/workflows/*.yml` is CLEAN end to end** -- the one surface of the four with no residue.
Every symbol, ctest case name, doc path and preset name it references resolves.

- [ ] **The rule to apply:** delete it if its only justification is history. Keep a guard only when it
      catches a mistake somebody can still make today.

**Two things deliberately NOT filed as legacy, with the check that settled each:**

- `KICKOS_MIN_STACK_SIZE`'s `#ifndef` default in `config/system.h` survives: it is reachable when
  `KICKOS_ARCH` matches none of the six ladder arms, which is a NEW PORT and a mistake somebody can
  still make today.

**One maintainer decision, not an evidence gap -- DECIDED 2026-08-05: DELETE.** The gate on the
root-MMIO service list was an empty list plus a `FATAL_ERROR` that could not fire on any
configure in the tree. Its comment argued it should stay for the next board whose bring-up writes
MMIO from root. **The maintainer's ruling: that class of misconfiguration cannot arise under the
M4.7.5 mechanism unless a user genuinely asks for it, and the tree will not carry hand-rolled
catchers for every misuse when a proper framework is coming.** 22 lines deleted; four enforcing
configurations still build, including `frdmk64f` on its full service list, which is the board the
gate used to police.

**A gate blind spot found while doing it -- FIXED 2026-08-13 in M4.8.4.** The gate built its
valid-identifier set from every tracked NON-markdown file, and the audit ledger then tracked as
the audit ledger then tracked under `docs/` recorded this name, so **any name it recorded stayed valid**
even after the build dropped it -- a DOCUMENT behaving as a source, which is wider than the stale
source comment the behaviour was designed around. The fix excludes `docs/` from the identifier scan
only; the corpus being CHECKED is unchanged at every tracked `*.md`. It turned 39 markdown references
to this knob and to the privileged-root one RED, and the doc audit repaired all 39 by rewriting the
claim without spelling the dead name -- fencing the one that is a verbatim capture line.

## Standing decisions and the M4.5 queue

Kept for the decisions, which still stand, and for the queue's steps still open.

### Decisions taken (do not re-open without new information)

- **`kos_reboot` shares `kos_shutdown`'s authority** (`AUTH_SYSTEM`), rather than taking a capability
  at a reserved index. The reserved spare therefore **stayed free**, which is worth more than bit
  granularity: spending the last well-known index forces the next one to raise
  `KICKOS_CAP_FIRST_DYNAMIC` and costs a dynamic slot on every board. (The spare has since been
  deleted outright and `KICKOS_CAP_FIRST_DYNAMIC` lowered to 2, which only strengthens this.) **LANDED** as
  `KOS_SYS_REBOOT`; recorded in full in the `kos_reboot` section below and in
  `docs/design-unprivileged-root.md` section 9. The stage-4 re-cut renamed that shared bit from
  its old device-authority spelling without splitting reboot from shutdown.
- **`kos_ram_alloc` gets an explicit self-grant**, not an implicit one at alloc: `AUTH_MEMORY`-gated,
  bounded by `KICKOS_MPU_MAX_REGIONS`, failing loud with `-KOS_ENOMEM`. **LANDED** as
  `KOS_SYS_MEM_SELF_GRANT`.
- **The selftest gate asserts a named, posture-dependent expected-skip list**, not a skip budget.
  **LANDED** as `EXPECT_SKIPS`.
- **The service capability index is retired.** The ABI is not stable yet, so a superseded spelling
  is deleted rather than deprecated. **LANDED.**
- **clang-format is decided against** as a gate -- see the CI-hygiene section.
- **The record cites closing commits by SUBJECT, not by hash.** Hashes move under rebase; this
  branch proved it (the A/B witness hash `a463ab9` had to be re-resolved to `22e1c5a`).
  **SUPERSEDED** by the squash: subjects do not survive one either. The record names a single
  resolution target instead, `backup/m4.5.1-pre-squash`.
- **The canvas is mirrored into the repo** so git carries its history, the Cursor path staying the
  live file. **SUPERSEDED TWICE**: the record became an audit ledger edited in place, with no live
  copy outside the tree and no mirror, and that ledger has since been deleted
  (recoverable from git at `b27a409e`). The `.canvas.tsx` survives only on
  `backup/m4.5.1-pre-squash`.
- **Non-goals are appended to the existing `## North star` section of
  `docs/reference/architecture.md`**, not given a new document, because that section already states
  all three goals. **LANDED** as `### Non-goals -- seL4 machinery deliberately NOT adopted`
  (`m4.5.1: state the seL4 machinery KickOS does not adopt, and the arithmetic refuting it`,
  79b7a37): all four, each with its arithmetic.
- **Sequencing: M4 driver breadth and M6 SMP wait behind goal 1** (fleet flip,
  `arch_periph_enable`, `kos_cap_narrow`). Both multiply capability and memory complexity on a
  fleet that at the time still defaulted to privileged root, so doing them first would have widened
  the surface that goal 1 then has to confine. **LANDED** in `roadmap.md` under `## Next`
  (`m4.5.1: put M4 driver breadth and M5 SMP behind goal 1`, a5fc422). The premise is now DISCHARGED
  rather than pending: M4.5.6 deleted the privileged-root posture knob, so goal 1 holds
  unconditionally and
  M4.6's driver work is what comes next.
- **Selftest tiering (core tier + optional tiers) was to be considered for the two 64 KiB boards --
  but MEASURE FIRST, and the measurement says it is not needed today.** See below.

### The tiering measurement (this is the "measure first" result)

The premise was that the fleet defaults to `Debug` with no optimisation, inflating flash ~26%. It
WAS handled by a two-board `-Os` block applied under `KICKOS_ENABLE_SELFTEST`, since DELETED and
subsumed by the fleet `MinSizeRel` default (see the closed item below). No `-Os` appears in the root
`CMakeLists.txt` any more. At branch tip (`text + data`, against 64 KiB of flash):

| Board | flash used | free | headroom |
| --- | --- | --- | --- |
| `bluepill-c8-st` | 51,540 B | 13,996 B | 21.4% |
| `f302nucleo-st` | 51,716 B | 13,820 B | 21.1% |

**Tiering is unnecessary: both boards carry the fleet-uniform suite with ~14 KiB spare.** The
revisit trigger ("only if headroom actually erodes") has **partly fired** -- 1,636 bytes went on
`bluepill-c8-st` and 1,644 on `f302nucleo-st` against the M4.5.1 merge baseline, attributed
symbol-by-symbol in archived `M4.5_footprint_meas.md` section 9: two new selftest cases
(`t_bus_device_slots` + helpers, `t_reboot_denied`), a real +88 in `domain_for` from the
one-holder-per-MMIO-window check, and `MAX_TESTS` 64->128, whose 512 bytes land on RAM rather than
flash. 21% is still ample, so tiering stays unbuilt.

**The narrower N16 question now has a measured answer: per-preset build types**
(archived `M4.5_footprint_meas.md` section 10). `CMAKE_BUILD_TYPE=MinSizeRel` is
byte-identical in `.text` to an explicit `-Os`, and its `-DNDEBUG` is inert in this tree -- there is no `NDEBUG`
reference anywhere and no `assert()` outside the `KICKOS_DEBUG` guards -- so its only cost is
losing `-g`, recoverable with `-DCMAKE_C_FLAGS_MINSIZEREL="-Os -DNDEBUG -g"`. It also **inverts the
block's stated cost**: `CMakeLists.txt:139-140` gives that cost as "the `-st` kernel is no longer
codegen-identical to the same board's non-st build", and measured kernel-side on `bluepill-c8`
`hello` the two differ by **13,505 bytes as shipped** and by **114 at `-Os`** (the flag's genuine
content). The unoptimised default is what creates the divergence; widening `-Os` nearly closes it,
which matters because silicon witnesses are taken on `-st` images. **LANDED**: the four MCU base
presets and the sim preset build `MinSizeRel`, `-g` is re-added under that config, and the RP2040 /
RP2350 optimised-build defect that blocked it is fixed. The consequence for the existing witnesses
is the fleet re-witness pass under M4.5.5.

### One new finding: guards that exist but assert almost nothing

Filed as one item because the **pattern** is the point -- a check that is present, green, and
carrying almost no information is worse than an absent one, because it consumes the attention that
would have gone to writing a real check.

- [ ] **Replace the `.bss`-emptiness linker assert and the vacuous `kernel_ctor_placement` with one
      post-link ELF check.**
      - `ASSERT(_ebss > _sbss)` in the linker scripts only fires when kernel `.bss` is **entirely
        empty**, which needs all four archive selectors to fail at once. It misses the far likelier
        **partial** case: one KickOS archive renamed, or a new one added and not listed in all
        **eleven** scripts. That library's writable state then sits silently inside the app's
        granted window -- an isolation hole that the assert reports as fine.
      - `kernel_ctor_placement` passes **vacuously fleet-wide** (same class: green, asserting
        nothing).
      - **Proposed fix, in an idiom the project already uses** (`check_kernel_ctor_placement.sh`,
        `check_oot_export.sh`): a post-link ELF check asserting that **no symbol from a
        kernel-owned object lands inside `[__kickos_appdata_start, __kickos_appdata_end)`**. That
        catches the partial case the linker script structurally cannot.
      - **Why not fix it in the linker script:** GNU ld cannot be asked whether an input selector
        matched anything, so the in-script version can only ever approximate. This is a limitation
        of the tool, not of the attempt -- worth recording so the next person does not retry it.

### Remaining queue, in dependency order

Ordered so each step's input exists when it starts. Items 1-2 are cheap and unblock judgement;
the sweeps go last because they touch everything and would conflict with any of the above. Closed
items keep their number and are struck through rather than removed, because they are cited by
number: the record and the XMC entry under Blockers below both point at **item 5**.

1. ~~**Measure `-Os` on the tight boards**~~ -- DONE above. Outcome: **tiering not needed.** The
   narrower question of how `-Os` should be expressed is answered above too, and landed:
   per-preset build types (`MinSizeRel`).
2. **Selftest tiering** -- **do not build it.** Headroom has since eroded by ~1.6 KiB to 21.4%,
   which is a partial fire of that trigger and still ample. Kept in the queue only so the next
   reader sees it was considered and refuted by measurement, not forgotten.
3. ~~**Non-goals into `docs/reference/architecture.md`, appended to the existing `## North star`
   section.**~~ -- DONE (79b7a37). All four landed with the arithmetic that refuted them (no untyped
   memory / `Retype`; no CNodes; no derivation tree; no per-instance capabilities), under
   `### Non-goals -- seL4 machinery deliberately NOT adopted`, and the common thread is stated: the
   16-slot ceiling with the tiny boards' tables under it. **That section's `read`/`open`/`socket` sentence
   stays alone** -- the maintainer reads it as a design rule, not a status claim. It was not
   touched, and should not be by a later pass.
4. ~~**Record the sequencing note** (M4 driver breadth and M6 SMP behind goal 1) in `roadmap.md`.~~
   -- DONE (a5fc422), as a block quote under `## Next`.
5. ~~**Move the XMC USIC bring-up into the granted driver thread, and add the privileged configure
   seam it needs for FDR/BRG/CCR.**~~ -- DONE in M4.5.6 (`KOS_SYS_PERIPH_REG_WRITE = 42` /
   `arch_periph_reg_write`, an exact `(base, offset)` allowlist, possession-gated), both parts:
   `xmc_spi0_start` now contains zero register access. The three PV-write-only stores were a measured
   hardware refusal and the seam is witnessed discriminating against them on silicon -- see the entry
   under Blockers below. It closes the consequence too: `xmcssc` AS A SERVICE is witnessed at
   `commit 270b6fa` and `kickos_services_xmc4800relax` came off the root-MMIO
   refusal list, which was emptied then and has since been deleted outright.
6. **`stm32f103` `arch_mpu_min_region()` override.**
7. **Re-point `kernel_ctor_placement` at the `cxxtest` ELF** (it is vacuous where it is now; see
   the finding above -- these two are the same problem and can land together).
8. **CI hygiene set, minus clang-format** (which is decided against).
9. **Branch-wide comment sweep** -- last, because it touches everything.
10. **Commit-message reword as a SEPARATE step after the sweep**, not folded into it.
11. **Record citation pass** -- after the record edits, so it runs over the final text.

## M4.5.1 -- kernel audit follow-ups (2026-07-26) -- COMPLETE except S4 (2026-07-27)

Findings from a code audit of the kernel, all rated **Medium or below** -- none is a live
escalation or a fleet blocker. They are bound-the-unbounded / be-honest-about-the-error
hardening, roughly ordered by exposure. Six of seven landed; the seventh turned out not to
be implementable where it was filed, and says so in code.

Verified on the host sim and QEMU only, each with a gate checked to FAIL first. **These commits
have not yet been through CI** -- but the branch under them has: the maintainer reports **CI green
at `16d89a0`**, the tip before this batch, covering the branch's first 33 commits. Every commit
that touches `.github/workflows/` is at or before it, so the fleet-wide `-Werror` and the rest of
the pipeline are now observed on CI's pinned 15.2.rel1 rather than argued from a local 15.3.rel1.
Uncovered: everything after `16d89a0` -- stage 0/1 of unprivileged-root, this batch, the book
work, the stage-2 flip and its silicon record, and the record passes over all of it. That is
**49 commits as of this commit**, derived as `git rev-list --count 16d89a0..HEAD` on a branch that
stands 82 commits from `master`, with `16d89a0` at position 33 by the same count against
`master..16d89a0`. **Take the command over the number**: it grows with every commit until CI runs
again. The figure this replaces (21) was correct at `3e8ed10` and went stale when the two transport
branches were replayed in, which is the whole reason the method is written down here. CI status
itself is the maintainer's report, not checked from here (`gh` unauthenticated, and `ci.yml`
triggers `push` only on `master`).

- [ ] **Implement `__malloc_lock`/`__malloc_unlock`** -- **NOT DONE, and it cannot be done
      here.** `m4.5.1: document why the malloc lock stays a no-op` (291815c)
      replaces the vague FOOTGUN comment with three measured facts: newlib takes this lock
      RECURSIVELY (in the linked `cxxtest` image `_free_r` holds it and calls
      `_malloc_trim_r`, which takes it again), so a non-recursive lock self-deadlocks and a
      re-entry detector fires on a legitimate free; a recursive lock needs thread identity and
      userspace has none; and capabilities are per-task, so there is no lock object two threads
      can name and no reserved index left to seat one at. "An IrqLock-equivalent" is not
      available -- this file is userspace, which is the whole point of the surrounding
      milestone. Real fix: the per-thread libc state / TLS item under "Later -- not M1", or a
      kernel-held lock behind a syscall (a designed change).

## M4.5.1 -- found during the CI / out-of-tree hardening work (2026-07-26)

- [ ] **Re-point `kernel_ctor_placement` at the `cxxtest` ELF.** The gate passes fleet-wide, but
      vacuously: every app it inspects links an empty `.kickos_app_init_array` window, so the
      script takes its early-out without ever dereferencing a pointer. `cxxtest` is the one image
      with real app ctors -- point the gate at it so it actually asserts something.
- [ ] **Console bytes lost on shutdown.** On a service-list board, root returning while the
      userspace driver still holds queued bytes loses them: `console_tx_flush_sync()` is a no-op
      (the ring was disarmed by `console_tx_deinit`) and `arch_shutdown` then spins forever.
      Shutdown has to drain through the owning driver, not the retired kernel ring.
## M4.5.1 -- found during the kernel-audit batch (2026-07-27)

- [ ] **The resume barrier has never been observed to spin.** Bounding
      `wq_confirm_resume` (6961989) needed a gate, and calibrating one measured that the loop
      takes **zero iterations on the host sim AND on qemu armv7m** across the whole suite: a cap
      of one iteration does not fire. The sim is expected (its switch is synchronous inside
      `wq_block`), but ARM is not -- the long comment at `sync.h` explains that `arch_switch`
      only PENDS PendSV and `arch_irq_restore` has no ISB, so 1-2 instructions retire on the
      not-yet-switched thread. Either the switch always lands before the caller reaches the
      barrier (a call and a return later), or the window is narrower than the comment implies.
      Worth settling, because the barrier is on the mutex/endpoint wake path on every board and
      is currently justified by an argument nothing exercises. Silicon witness still owed --
      timing is exactly what an emulator does not reproduce.
- [ ] **LTO does not link, on any board.** `-flto` fails every app with
      `(.isr_vector+0x4): undefined reference to Reset_Handler`. The handler is defined in a C++ TU
      and referenced **only** from the vector table in an assembly object, so the LTO plugin sees
      no reason to keep the definition (measured on `bluepill-c8`; the defect record is
      `docs/design-flash-footprint.md`, *LTO does not link*). So LTO is not an available footprint
      recovery today. It is not on the `-Os` path
      N16 needs, so it gates nothing -- filed so it is not rediscovered. **Record only: no fix
      attempted.**
- [ ] **`kickos_core` no longer carries the archive group.** c539d1c moved the RESCAN group onto
      the leaves, and it rides `KickOS::kernel`, the one leaf left. A consumer linking
      `kickos_core` DIRECTLY gets usage requirements but no archives. The
      documented contract already says consumers link a leaf and never core, and both
      out-of-tree export gates pass -- but core is still in the export set, so the contract is
      now load-bearing where it used to be advice. Either state it in the exported package or
      make linking core alone a configure-time error.
- [ ] **Reclaim `arch_ram_alloc`'s alignment run-up** (allocator work). The bytes skipped
      ahead of each allocation to satisfy its alignment are dropped on the floor -- which is why
      boot-stack allocation *order* is load-bearing today (idle must be allocated before root).
      Folding the run-up back into the free space removes that ordering constraint. Subsumed by
      the general freeing allocator under "Later -- not M1".

## M4.5.1 -- CI hygiene (2026-07-26)

- [ ] **Add link-only CI jobs for `f302nucleo-st` and `bluepill-c8-st`** (maintainer-confirmed
      2026-07-27). CI builds only the plain presets, so a selftest image that overflows 64 KiB of
      flash goes unnoticed until someone builds the `-st` preset by hand. These two boards are
      build-only for the suite anyway -- a link check is exactly and only what they provide, so a
      link-only job is the whole value at none of the runtime cost. Both link today with ~14 KiB
      spare (measured in the session record above, and shrinking), and the job is what keeps that
      true. Note for whoever adds it: the base presets build `MinSizeRel` fleet-wide, so the job
      still has to configure the `-st` VARIANT -- only the selftest image risks overflow. The
      per-board `-Os` block this note used to name is gone. A local sweep of all
      thirteen `-st` presets found one real link break that the seven emulator gates could not
      (`esp32-wroom-st`, Xtensa, missing `kickos_arch_mpu_commit`), which is the argument for
      widening this beyond the two tight boards later.
- [ ] **Add a licence-header gate.** The premise that it could wait -- "coverage is 100%, so this
      is cheap to hold" -- turned out to be false: re-measuring on 2026-07-27 found
      `docs/design-rp2350-mpu-armv8m.md` carrying no SPDX identifier while all 26 sibling design
      records did. Header added, so coverage is 534 of 536 tracked non-binary files (the rest are
      `.gitignore` and the six JSON presets, none of which can carry a comment). The drift the gate
      exists to catch had already happened unnoticed, which is the argument for adding it now.
- [ ] **Pin GitHub Actions to commit SHAs**, not moving tags -- a tag is a supply-chain seam
      controlled by someone else.
- [ ] **Wire the telemetry runtime gates into CI.** The `sim-telem` / `qemu-telem` presets exist
      and work, but no job runs them, so telemetry can rot without anything going red.

## Unprivileged ctors and `main` -- start unprivileged, holding capabilities (2026-07-27)

**Reasoning, blockers and the boards this does not work on are now filed as
`docs/design-unprivileged-root.md` (ACTIVE).** This section stays the actionable checklist.

A design investigation superseded stages 2-8 of the old plan: root should **start unprivileged
holding capabilities** rather than start privileged and demote, so there is no demotion to build.
`thread_regions_recompose`, the drop-privilege syscall, its per-arch backends and Xtensa-last are
**deleted, not deferred** -- the region set is composed once in `thread_create`
(`kernel/thread/thread.cc:94-134`) from a privilege that never changes, and the rest existed only
to manage a transition this design does not have. The reason, recorded once: **every ISA with a
ring split already encodes thread privilege in the fabricated first frame and restores it on the
first switch-in** -- armv7m `ctx.npriv` (`arch/arm/armv7m/arch_armv7m.cc:111-119`), armv6m
(`arch_armv6m.cc:102-108`), rv32imac `MSTATUS_MPP` (`arch_rv32imac.cc:143-158`), rxv3
`PSW_THREAD_USER` (`arch_rxv3.cc:279-289`). The two ports without a ring split store nothing:
xtensa (`arch_xtensa.cc:271-273`) and the sim, whose `arch_context_init` takes `privileged` and
**discards** it (`sim.cc:758-761`) -- privilege there is the thread's region set plus a
per-context mid-syscall raise counter, so the sim has no CPU-mode axis at all. Starting root
unprivileged therefore needs **zero new assembly on any port**, and Xtensa comes along free
rather than last. `drop_priv` survives only as a **contingent, much smaller** item: it is the
only mechanism giving "privileged bring-up then self-confinement for life", which is what the
blocked bring-up bodies below want -- in scope only if the `arch_periph_enable` seam (stage 3)
proves insufficient.

The old stage 1 (arena-allocated boot stacks) LANDED on the M4.5.1 branch -- see
`m4.5.1: take the root and idle stacks from the arena, not .bss`. The new stages, in dependency
order:

**Stage 0 -- independent prerequisites, no behaviour change. COMPLETE** (all three were real bugs;
each landed with its own gate, and the whole stage costs 276 B of flash and 8 B of `.bss` on
frdmk64f+blink -- the 8 B being the argv struct itself).

**Stage 1 -- the authority capability, root still privileged. COMPLETE** (see
`m4.5.1: gate the eight authority syscalls on a capability, not only on privilege`; +374 B flash
on frdmk64f+blink, no `.bss`).

**Stage 2 -- flip per board**, behind a build-enforced posture knob (default ON,
**NOT a weak symbol**: opting out of the boundary had to be visible in the board's build, not
silently satisfied by a link-time override).
**The knob NO LONGER EXISTS -- M4.5.6 deleted it and root is now unconditionally
unprivileged.** Everything in this stage record is dated history, and no item under it describes a
posture that is still selectable.

**Stage 3 -- the blocked bring-up bodies. COMPLETE** (2026-07-29, `127efb5`).

**Stage 4 -- the app story. COMPLETE** (three commits: the delegation type guard, the re-cut plus
`kos_cap_narrow`, then the narrow site plus the per-app declarations).

Opened by stage 4:
- [ ] **A per-service authority declaration in `kos_service_cfg`.** The struct has `rsv[4]`, so a
      byte fits with no layout change, and the runner could then narrow *between* entries -- hold
      `AUTH_CONSOLE` only while the console service kind entry runs. Deliberately NOT done in stage 4:
      root holds `CAP_AUTH_ALL` for the whole list run either way, so the only window it closes is
      between one bring-up entry and the next, with no app code running, and the app-level narrow
      already strips the bit before `main`. It becomes worth doing when service bring-up moves off
      the root thread -- i.e. with the item directly below.

Carried over from the old plan, untouched by this design:
- [ ] **Move app bring-up into the service lists**, so an app is started the way a driver is.

Blockers and limits:
- **One service bring-up body used to poke MMIO directly from root -- CLOSED by M4.5.6's
  `arch_periph_reg_write` seam, and `xmc_spi0_start` now contains zero register access.** The
  consequence is discharged too: `kickos_services_xmc4800relax` came OFF the root-MMIO refusal
  list (emptied then, deleted outright since), `xmc4800-relax` defaults to its full service
  list under enforcement, and `xmcssc` AS A SERVICE is WITNESSED -- both driver banners on the wire
  at `commit 270b6fa`, no dark board. Recorded in full under M4.5.6. The analysis below is kept
  because it is what the seam had to satisfy.
  `system/driver/xmc4800/xmcssc/spi_usic.cc` (USIC kernel clock, baud, protocol) -- on
  `xmc4800-relax`, the enforcement flagship. The two K64F bodies were **retired by stage 3**:
  the polled UART0 console driver, since removed (AIPS PACR), and
  `system/driver/mk64f/k64dspi/k64dspi.cc` (clock gates, pin mux, GPIO, DSPI config) each call
  `arch_periph_enable` from the driver thread that holds the window. Stage 3 does **not** cover the
  XMC, which needs USIC-specific FDR/BRG/CCR programming rather than
  "ungate a clock, drop supervisor-protect", so `xmc4800-relax` stays console-only under the flip.
  **The XMC blocker is hardware, measured on silicon
  2026-07-28** by `user/apps/xmc4800-relax/pvprobe`: an unprivileged thread holding the MPU grant
  for the U0C1 window (`0x4003_0200`) has its writes to FDR/BRG/CCR **silently discarded** (no
  fault, read-back unchanged), while `SCTR` (`U,PV`) in the same window in the same run lands
  exactly and an ungranted SCU poke MemManages. So the window is grantable and the *transfer* path
  works unprivileged -- `xmcssc` already proves that -- but `xmc_spi0_start`'s three PV-write-only
  stores need a privileged executor, and the flip needs a seam for them. Given that seam the
  bring-up moves **wholesale** into the granted driver and must, because no path exists by which a
  post-flip root holds a DEV region: `ARCH_MPU_DEV` is attached only in `domain_for`, reached with
  MMIO only from `thread_create_call`, and `KOS_SYS_MEM_SELF_GRANT` hardcodes `ARCH_MPU_R | ARCH_MPU_W`.
  So the driver is the only possible caller of the seam. The
  earlier entry here said the opposite ("contradicted, not untested", from `consoledemo`'s scrambler
  garbling the UART); that was **invalid inference** -- the scrambler also writes SCTR/TCSR/PCR (`U,PV`) and
  gates `KSCFG`, any one of which garbles the UART on its own. Also corrected: Table 18-20 marks
  exactly three registers `Write = PV` (FDR, BRG, CCR); `INPR` is `U,PV` and its earlier inclusion
  was a transcription slip (untested here). It was **enforced
  at configure time** (the root-MMIO service-list refusal) rather than left to fail on the hardware,
  because the runtime failure is a fault mid-bring-up *after* the console has been relinquished, i.e.
  a silent dark board. **The condition is now `KICKOS_HAVE_MPU`, not the posture** -- M4.5.6 deleted
  the posture, and the substitution is the right one on its own merits: with the MPU off an
  unprivileged root DOES reach MMIO (measured), so the gate's subject is enforcement. The XMC listing
  is lifted, so the enforcement image links AND calls `xmcssc`; the empty list and its gate remain for
  the next board whose bring-up writes MMIO from root.
- **A published console drops `kos_print`, and there is a real dark window** -- narrower than this
  entry used to claim, and the narrowing is the point. What drops is the KERNEL bring-up path:
  `kos_print` / `kos_kconsole_write` hand bytes to the kernel console, and `console_emit` drops all of
  them once the UART is `USER_OWNED` (RTT still carries them where it is built in). **`printf` and
  `std::cout` do NOT drop.** THREE publish-aware writers exist and are kept in step
  (`user/include/kickos/sys/emit.h:11-12`): `kickos::emit()`, `tests/tap/tap.cc`'s `emit()`, and libc
  `_write` (`user/src/newlib_stubs.cc:19-59`, which tries `kos_send` on cap 0 and falls back for the
  remainder). So a user app on the standard APIs reaches a published console driver, and this is NOT a
  reason to hold USB CDC or anything else. Corroborated on silicon 2026-07-30: `frdmk64f` on its
  published full service list carries `# tap route: stdout endpoint -> console driver (service list
  published)` and still delivers its whole selftest, against `pizero2350`'s
  `# tap route: kernel debug console (stdout not published)`.
  Two real things remain. A freestanding app that uses `kos_print` instead of the publish-aware writer
  loses its output -- found on silicon: the first `rootfault` capture held a fault dump with nothing to
  check it against, and `mpu_fault`'s captures had been marker-only on every service-list board, both
  fixed via `kickos::emit`. The open question of which other worker-printing diagnostics share it is
  **answered: `pvprobe` and `inprstorm` do** -- filed below. And there is a genuine DARK WINDOW between
  the publish and the driver actually serving cap 0 (in the polled console drivers, since removed), which is
  ordering rather than a writer choice and is owned by M4.6.1.
- **The panic-path UART reclaim clips bytes in flight.** `kpanic_enter` takes the UART back from the
  userspace driver so the report always reaches the wire, which works, but on `xmc4800-relax` it
  reproducibly garbles roughly the last 8 bytes the driver had queued (the polled TX word pending in
  `TBUF0`). Cosmetic for a terminal report, but it eats the tail of the line preceding the dump.
- **`bluepill-c8` and `f302nucleo` were held by the absence of a RING-ARM witness, not by RAM or
  handles -- and `f302nucleo` has now TAKEN it.** `ringpriv` reports `PASS (5 arms)` there on real
  no-MPU armv7m silicon (M4.5.6, `commit 270b6fa-dirty`); `hello` and `stress` also pass at 2 KiB of
  heap (`docs/reference/boards.md`). `bluepill-c8` remains unwitnessed on the ring arm, but the prober
  now exists, so what it needs is a board and not code.
  Both are armv7m, so the flip's mechanism (`ctx.npriv` in the fabricated first frame) is present;
  neither part has an MPU (`stm32f103` none, and `f302nucleo` is the R8 `x8` line, which has none
  either), so stage 2's gate -- selftest green *under enforcement* plus a cross-domain `rootfault`
  -- cannot be met on either. The tiny boards' cap-table provisioning costs the flip nothing: the
  authority word is TCB state and spends no slot at all. The arena is heap policy, not the part:
  measured 6,560 B (`bluepill-c8`, production image), 2,592 B (its selftest image), 14,752 B with
  the heap carve at zero; 8,512 and 4,512 on `f302nucleo` since its carve went to 2 K
  (archived `M4.5_footprint_meas.md` section 7, `docs/reference/porting.md`
  minimum-requirement). The "barely 3 KiB" reading is a selftest-image figure.
- **`Thread::privileged` survives**, with narrowed meaning: it selects the memory posture (kernel
  domain + permissive background), it is the confused-deputy bypass at `syscall_mem.cc:37`, and it
  stays the home for "may spawn a privileged child" -- which should NOT be a capability, since
  holding it is equivalent to holding everything forever. Consequence: on a root-unprivileged
  board, **no privileged thread can come into existence after boot**.
- **`idle` stays privileged and holds no capabilities** -- it runs no app code, and RXv3 `WAIT` is a
  privileged instruction while RISC-V U-mode `WFI` is optional per spec.
- **The reserved cap index range was to be full after this** (0 stdout, 1 clock, 2 authority,
  3 spare -- reboot shares shutdown's bit, so index 3 stays free). **It did not end there**: the
  authority cap type was deleted and the word moved to `Thread::authority`, so the reserved range
  is `KOS_CAP_STDOUT = 0` and `KOS_CAP_CLOCK = 1` only, with `KICKOS_CAP_FIRST_DYNAMIC` at 2
  (`system/include/kickos/sys/cap_index.h`, `cmake/cap_geometry.cmake`); see the `kos_reboot`
  bullet above and the SUPERSEDED-in-mechanism note below. **The five-bit authority ceiling is
  gone**: the width is bounded by `kos_thread_params::authority` (a `uint8_t` in padding) rather
  than by a capability entry. Two more authorities cost nothing; a ninth needs that params field
  widened.
- **Delegation packing collides with reserved names** -- spawn delegation puts cap *i* at child
  index *i+1*, so a delegated cap lands at index 1 (`KOS_CAP_CLOCK`) and a second at index 2. The
  authority cap can no longer be the one that collides (refused by type at the delegation site), but
  the `KOS_CAP_CLOCK` aliasing still blocks the narrowed hand-off to a driver manager until the
  deferred explicit-destination-index work lands.
- **Cap-gen is a `uint16_t`** with no object generation behind a poolless cap, so 65536
  close/re-seat cycles wrap it. Unreachable in-tree; same unbounded-counter class as the
  domain-refcount item above.

## M4.5.5 -- MPU region-encoding classes

Ordered after stage 4 (`kos_cap_narrow`) and before the posture-knob deletion, but
**not a blocker for it** -- the knob went away on the strength of the flip, not of region shaping,
and it is gone as of M4.5.6. One general fleet re-witness pass closes the step; it comes due with
the rest of the bench debt under M4.6.3..N.

- [ ] **One general fleet re-witness pass, and it closes the step.** Every silicon witness in
      `docs/reference/boards.md` was captured from an UNOPTIMISED binary: the MCU presets built
      `CMAKE_BUILD_TYPE=Debug`, which is `-g` with no `-O` flag at all, and the fleet now builds
      `MinSizeRel` (`-Os -g`), which moves each image by roughly 17 to 23 KB. Per-board figures in
      archived `M4.5_footprint_meas.md`.
      **What does not transfer, and must be re-captured:** fault addresses, disassembly offsets,
      symbol sizes, stack-depth observations, and every timing figure -- the bench numbers, and
      `inprstorm`'s measured ~37,700 ISR invocations/second
      (`user/apps/xmc4800-relax/inprstorm/main.cc:25`). **That figure is distrusted for a SECOND,
      independent reason**, recorded under M4.5.6's `inprstorm` item: it was one operating point, and
      the FIFO-storm profiles replaced it with a structural bound (`min(fill, drain)`), which no
      re-measurement can move. So the toolchain caveat here and the operating-point caveat there are
      separate; the M4.5.6 captures were already taken `MinSizeRel`, which settles this one for that
      app but not for the rest of the list. Least transferable of all: `pvprobe`'s
      privileged-write measurements, whose whole subject is whether an individual store lands.
      **What stands:** `bluepill-c8-st` and `f302nucleo-st`. The deleted two-board holding measure
      already built those two `-Os`, and their `.text` is byte-identical under `MinSizeRel`.
      **What the move buys, which changes what a witness is worth:** a board's `-st` kernel and its
      non-`-st` kernel differed by **13,505 bytes** as shipped, so an `-st` witness never
      transferred to the shipped image at all. Both now build `-Os` and differ by under 200 bytes
      -- the flag's genuine content (`arch_reboot`, `arch_irq_inject`, `arch_mpu_probe_addr`,
      `irq_spurious_count`) -- so an `-st` witness finally does transfer.
      **The same pass clears this milestone's silicon debt.** Each item is unwitnessed for its own
      reason and none of them justifies a separate bench trip:
        - The UART-FIFO drain on the reboot path (`arch_console_flush_sync` before `arch_reboot`,
          `kernel/syscall/syscall.cc:389`). Only `mk64f` and `xmc4800` implement the seam and
          neither has an emulator gate, so the sim and QEMU gates exercise only the no-op fallback
          (`arch/common/arch_console_flush_sync_default.cc`) -- the truncation fix itself has never
          run against a real
          UART.
        - `dev_window_exclusive` and `bus_device_slots`: both postdate every silicon capture, so no
          chip has ever run them. Already recorded under the five-apps DEV-window item below.
        - The optimised `arch_reboot` path on `picopi` and `pizero2350`: verified by disassembly
          only, never executed, and neither RP part has an emulator gate. Distinct from the
          never-run RP2040 and imxrt1062 reboot BACKENDS under *Needs hardware* below.
        - `f302nucleo` joins the bench this round: it has an onboard ST-Link and a VCOM console,
          and `tools/flash-stlink.sh:18` already defaults `--connect-under-reset` on for it. It is
          the **only physically-present no-MPU ARM board**, which makes it the sole possible
          silicon witness for the claim that unprivileged root is real on a part with no MPU --
          root starting unprivileged, the ctors and `main` running, selftest green. That is a
          declared objective of the pass, not a by-product. It is NOT the stage-2 enforcement gate,
          which no MPU-less part can meet (see the `bluepill-c8` / `f302nucleo` bullet above).
          **Taken early, in M4.5.6**: `f302nucleo` witnessed the no-MPU claim (`selftest` `1..63` all
          passing) and the RING arm besides. Its fault-reporter root cause was the last thing it
          owed this pass, and that is CLOSED (2026-08-13): the flash command, not the firmware.

## M4.5.6 -- delete the privileged-root posture knob -- and M4.5.7 -- remove the weak-symbol seam mechanism -- BOTH COMPLETE (2026-07-31)

**TWO cleanup sub-milestones, in this order. BOTH ARE DONE** and merged, squashed into `dde73ca`
(PR #6): M4.5.6 deleted the privileged-root posture knob, M4.5.7 removed the weak-symbol
seam mechanism. The order was
not a preference: the knob deletion removes a posture and every `#if` branch behind it, so the seam
pass edited a one-posture tree instead of two. Both are foundation work ahead of M4.6 and
neither is a driver feature. Next is **M4.6.1 -- IRQ + console visibility**.

**Half one -- delete the privileged-root posture knob. COMPLETE** (2026-07-30/31, MinSizeRel,
merged in `dde73ca`; developed as `c5d9b0d` -> `270b6fa` -> `124b68c`, which the captures
stamp). The first
inventory below was written at `c5d9b0d` and has been corrected against the two later commits.
**Capture hygiene, recorded because this milestone broke its own rule.** It wrote down "commit before
a witness pass" and then took most of the later captures at `270b6fa-dirty` or `2fc7799-dirty`
instead of at a committed tip. Only `b4-fault.log` and `b4-ringppb.log` stamp `124b68c`; the `b2-*`
set stamps a clean `270b6fa`. A `-dirty` stamp names the tree it was taken from, not a reproducible
one, so those captures cannot be re-derived from history.

Landed in `124b68c` (a few captures stamp the `270b6fa` working tree that became it), after the first
inventory above was written. Wire values live in `docs/reference/boards.md`; these are cited, not
duplicated.

- [ ] **`kos_thread_create` returns `-KOS_ENOMEM` for two different failures**, so arena starvation is
      indistinguishable from a legitimate pool limit at runtime:
      `kernel/syscall/syscall_thread.cc:306` is "thread pool exhausted" and `:375` is "stack arena
      exhausted". That ambiguity mislabelled
      **8 of `f302nucleo`'s 9 skips** as `SKIP pool too small` when every one of them was arena
      starvation, and it is what made the investigation above cost a session. Two distinguishable
      codes, or one diagnostic line naming which limit was hit, would have ended it immediately.
- [ ] **The structural coverage hole behind it: no emulated gate can exercise a buffered-ring panic
      flush, and the sim CANNOT substitute.** Every fault-dump gate in the fleet runs on an
      UNBUFFERED-console board (mps2 semihosting, `microbit`, `virt`), so "the panic path must drain
      a ring it just reclaimed" has zero in-env coverage. That is why the item above survived to
      silicon. **The sim looked like the fix and is not**: its synthetic TX backend re-raises SIGUSR1
      and re-asserts until the ring empties, so the ring is provably empty at panic time (`used=0`
      measured for both `fault` and `panicgate1`). MEASURED CONSEQUENCE: deleting
      `console_tx_flush_sync()` from `kpanic_enter` outright leaves the whole sim suite green. So the
      flush is unreachable-dead from the sim's point of view. `sim_published_panic` (M4.6.1) covers
      the reclaim and the polled route, not the drain.
      **WITNESSED on `pizero2350` 2026-07-31 (`.session/m458-silicon/pzdrain-*.log`), which is the
      first measurement of the drain with a PROVEN non-empty ring.** A burst-then-fault app with a
      probe inside `kpanic_enter` (after `arch_irq_save`, before the flush) read
      `used_at_panic=419` of a 511-byte usable ring, then 0 after the flush; all 16 burst lines
      reached the wire in order, dump last. Negative control with the flush deleted: the same 419
      stranded, only the 4 already-shifted bytes preceded the dump, and the remainder surfaced AFTER
      it spliced mid-token, flushed only by `kickos_bootloader_handover` on the way to BOOTSEL. On an
      image that halts instead of rebooting those 419 bytes are lost outright. The drain is therefore
      live and load-bearing on RP2350 even though it is dead code on the sim. Captures were taken
      from a dirty tree; the exact diff is pinned at `.session/m458-silicon/pzdrain-tree.diff`.
      Two claims in the tree overstate this and are owed a fix: `tests/integration/check_fault_dump.sh` (its
      header) and `user/apps/common/fault/main.cc` (its header) both say the marker catches a dump
      lost into an armed ring. On the sim it cannot. What `fault_dump` really covers is the
      `RECLAIMED`-to-polled routing.

**Half two -- remove the weak-symbol seam mechanism. COMPLETE** (2026-07-31, developed as an
isolated tip commit, merged in `dde73ca`). Outcome first, then the reasoning that chose it:

- **33 `__attribute__((weak))` definitions removed.** 32 were visible to a plain-fleet `nm` sweep;
  the 33rd, `SystemCoreClock`, is weak only under `KICKOS_BENCH=ON`, which is why the sweep missed
  it. 32 became one-symbol fallback TUs; `arch_mpu_apply` became a PLAIN, non-overridable definition
  because nothing ever overrode it -- `chip_mk64f.cc`'s claim that K64F overrode it was FALSE and
  has been corrected in the code. `.weak NMI_Handler` in 11 `startup.S` files became a file-local
  label. `SystemCoreClock`'s 0 now lives in the SIM arch library only, so an MCU chip that forgets
  it fails the LINK.
- **The placement rule is load-bearing.** Fallbacks live in `kickos_arch_<arch>` and NEVER in
  `kickos_kernel`: the rescan group scans the kernel archive BEFORE the chip archive, so a
  kernel-resident fallback is extracted in the same pass that first makes the symbol undefined and
  then collides with the chip. Seams the kernel declared moved out of `kernel/` into `arch/common/`.
  Canonical statement, stated once: `arch/CMakeLists.txt:11-71`.
- **The real invariant is ANCHORING, not a duplicate-definition error.** Reversing the rescan group
  still resolves correctly (the chip member is force-loaded in pass 1 via `-u g_isr_vector`), so the
  scan-order backstop was measured NEVER to fire. Proof it is insufficient: reversing the group AND
  moving `arch_idle_wait` into an unreferenced TU made the link succeed with ZERO diagnostics while
  `objdump` showed the fallback's `wfi` instead of the chip's `nop`. A silent decline.
- **Payoff realised**: `cmake/boot_arena.cmake` lost its weak/strong precedence logic entirely, and
  two backend definitions of one seam are now a `FATAL_ERROR` naming both files. The thread-stack
  pool modelling (`KICKOS_POOL_ARENA_ASSERT`) is untouched and scraped geometry is byte-identical on
  every board.
- **Two facts for the next porter**: ld's map headings are LOCALE-TRANSLATED (this host emits
  "Membre d'archive inclu..."), so the gate parses the map structurally; and `.text.<symbol>`
  sections do not exist in the hosted build or under the RX toolchain, which also prints map symbols
  without the psABI underscore -- hence the two spellings in the allowlist.
- **Gates**: +1 (`seam_defaults`) on every arm. sim 20, `qemu` 19, `qemu`+MPU 22, `qemu-m3`+MPU 20,
  `qemu-m7`+MPU 21, `qemu-m33`+MPU 21, `qemu-riscv`+MPU 16, `microbit` 13. Fleet links 33/33, MPU
  variants 14/14, zero `.data`/`.bss` change on `bluepill-c8-st` and `microbit`.

**Decision (2026-07-30): remove weak symbols from the arch/kernel seams entirely**, keeping only the
libc-interop exceptions. Two reasons, and the second is the deciding one:

- **Maintainability.** Resolution depends on archive member extraction and link order, neither of
  which is visible at the call site. `cmake/boot_arena.cmake` has to *reimplement* weak/strong
  precedence in CMake to model the boot arena, and that logic carried a latent bug: a `GLOB`
  attributed `arch/arm/common/arch_arm_pmsav8.cc` to the arch family though the build compiles it
  into the CHIP library, which was load-bearing on that file happening to decline the
  `arch_mpu_min_region` override.
- **`__attribute__((weak))` is a GNU extension**, and its interaction with archive extraction is
  implementation-defined. Requiring GCC for a kernel whose whole seam story is portability is the
  wrong trade. This already bit once, in-tree: GCC carries a weak attribute from a declaration onto
  a definition in the same TU, so every app authority override compiled `W` and link order
  picked the winner (`nm` was the check; reasoning was not).

**Inventory -- and the planning figure was WRONG, so do not repeat it.** The sweep counted 48
`nm`-visible weak symbols and inferred "45 of 48 go, 3 stay". Only 33 were ever convertible. The
true split:

- **32 `__attribute__((weak))` backend seams** -- converted (plus `SystemCoreClock`, bench-only and
  invisible to the sweep, and `.weak NMI_Handler` in 11 `startup.S` files).
- **3 libc-interop DEFINITIONS** -- `__dso_handle`, `__malloc_lock`, `__malloc_unlock`
  (`user/src/newlib_stubs.cc`), weak so a libc that *does* provide them wins. Allowlisted.
- **12 C++ vague-linkage COMDAT symbols** -- `kickos::List::push_back`, `SlotPool<T,N>::resolve`,
  `kickos::Kernel::Kernel()`, `kickos::emit`, ... They report `W` in `nm` but are LANGUAGE-MANDATED,
  not a seam mechanism, and are not removable. This is the whole of the counting error.
- **`kickos_app_build_time`** -- hand-rolled C vague linkage, not a seam: `user/include/kickos/app.h`
  defines it inline and the build force-includes that header into every app TU, so a lone TU cannot
  replace it.
- **7 weak UNDEFINED references** -- `__kickos_code_start/_end`, `__kickos_appdata_start/_end`,
  `_kickos_heap_start/_limit`, `__register_frame`. A fallback cannot supply an address for a symbol
  whose whole point is that it may be absent.

**Replacement: the lone-TU pattern, which this repo has already proven** in
the app authority default's own source file: the fallback sits alone in its own TU, so a chip defining
the symbol resolves it locally and the member is never extracted. Standard archive semantics, no
compiler extension, no weak attribute anywhere. The constraint is real and must be documented per
file: **such a TU must define exactly one symbol**, or it gets extracted anyway and collides.
Group two fallbacks in one TU only where they are genuinely all-or-nothing (`arch_diag_led_init`
plus `arch_diag_led_set`).
The two other proven alternatives stay available where they fit better: a CMake-selected provider
spliced into the link group (the init-provider cache variable), and no default at all where a missing
override should fail the link (`arch_reserved_blocks`).

**Payoff beyond the removal**: `cmake/boot_arena.cmake` loses its precedence logic entirely -- one
definition per link, found on the chip target.

**Honest about what this trades, not a lateral move.** The lone-TU pattern still rests on LINKER
behaviour rather than language semantics -- a member is extracted only to resolve an undefined
symbol. That rule is standard across every Unix-like linker and MSVC's `.lib` handling, whereas
`__attribute__((weak))` is a GNU extension whose archive interaction is implementation-defined. So
it is a real portability gain, but the seam is still not expressible in pure C.

**Non-regressible: the gate LANDED** as `tests/static/check_seam_defaults.sh`, ctest name `seam_defaults`,
registered in `user/apps/common/selftest/CMakeLists.txt` and running on EVERY board. It reads the
selftest ELF, that target's `-Wl,-Map` link map, `tests/static/weak_allowlist.txt`, every archive of the
rescan group and the app's own objects. Four legs, all four mutation-proved:

1. Each `*_default.cc` member defines EXACTLY ONE global symbol, no other member of the same archive
   defines it, and no fallback sits in `kickos_kernel`.
2. Where a backend defines the seam, the fallback member is ABSENT from the link map entirely. This
   is what makes the ANCHORING rule enforceable rather than commented.
3. Where none does, the fallback IS in the map's inclusion list, pulled by that exact symbol -- plus
   a non-vacuity check, so the gate cannot go quiet if a board resolves no seam from a fallback.
4. Zero weak symbols outside the allowlist, per archive AND in the final ELF; a `_Z`-mangled weak
   symbol in an archive must ADDITIONALLY be COMDAT (proved via `readelf` section-group `G` flags),
   so mangling is not a blanket exemption. Honest limit: section groups are resolved away in the
   final ELF, so mangled names are taken on trust there; the per-archive leg covers KickOS code.

**NOT in M4.5.5.** It touches every arch seam, so it would invalidate the silicon captures just
taken and blow the milestone's scope.

**Slot: M4.5.7, after M4.5.6, and BEFORE M4.6.** Foundation before the driver era, like the
rest of M4.5.x. Half one is the reason for the internal order (see the intro above). The ordering
against M4.6 is the CI gate: once zero-weak is enforced, every new seam is forced into the pattern on
first write. Were it placed after M4.6's driver work instead -- M4.6.1's IRQ substrate and M4.6.2's
USB CDC, the two sub-milestones that add the most new arch seams -- those seams would get written
weak because nothing stops them, and then be rewritten: the same work twice, plus a window where the
tree drifts back. `arch_periph_reg_write` under M4.5.6 is the pattern already being honoured ahead
of the gate.
It also overlaps the M7 seam rework -- `arch_ram_region_size` still carries its `SEAM (MMU era)`
marker -- but the three MPU-geometry seams (`arch_mpu_min_region`, `arch_mpu_region_pow2`,
`arch_mpu_region_encodable`) were converted here rather than deferred into that redesign: they are
fallback TUs now, so M7 rewrites bodies and not the resolution mechanism.

## M4.6.1 -- the IRQ substrate, then the buffered userspace UART on it

**First of the M4.6 sub-milestones, and the substrate the other two stand on.** The first three
items are filed in detail further down this file, so the detail is not duplicated here; the two
after them are carried up from `docs/design-m4-fable-review.md`, which recorded them nowhere else.
It goes first for two reasons: an interrupt-driven, respawnable console driver cannot be built until
the first three are fixed, and none of them needs a board on the bench.

**TWO HALVES, ruled 2026-07-31. BOTH ARE COMPLETE.** The buffered userspace UART
(`docs/design-m4.6-irq-driver.md` sections 7-8) is M4.6.1's SECOND HALF rather than a sub-milestone
of its own, so M4.6.2 stays USB CDC and M4.6.3..N stays the witness pass. The first half is
line-as-capability, reclaim on every death path, and handover ordering. The second half is the
chip-independent layer plus five per-chip consumers, four of which now carry the whole selftest on
silicon -- the per-board record is in *M4.6.1 IRQ consoles on silicon* below.

- [ ] **The clock-tree service contradicts its own bring-up DAG, and the DVFS notifier cascade has
      no timeout.** `docs/design-m4-fable-review.md` finding 6, OPEN, recorded nowhere else. Two
      defects in one principle. **The contradiction**: `docs/design-driver-era-scope.md` section 3.1
      makes CLOCK-TREE a persistent RUNTIME service that init brings up BEFORE gpio and the drivers,
      while its G7 dependency list has the clock-tree SERVICE follow the first drivers. Both cannot
      hold. Cheaper resolution recorded there: the DAG's real dependency is only "gate the driver's
      clocks at bring-up", a one-shot init step like pinmux, with no standing service. **The
      cascade**: "Linux CCF shape" hides that CCF notifiers are same-address-space calls under a
      mutex, where each notify here is cross-domain IPC. A rate change during an in-flight SPI EOQ
      or UART frame corrupts the wire, so the fan-out needs a PRE-quiesce (drain/park) plus a POST
      phase, which is a two-phase commit across N untrusted driver threads: one slow or dead driver
      stalls DVFS forever with no timeout, and a driver whose notify handler calls the clock service
      re-enters a single-threaded service parked mid-cascade. **Recommendation as recorded**: drop
      the standing clock-tree service from the M4 principle set, keep init one-shot gating (which
      satisfies the DAG) plus the G4 kernel mechanism with the console handshake as the FIRST forced
      instance of the notify protocol, and design the full service against that proven instance.
      **Outcome recorded 2026-07-30**: no standing service was built, which is the recommendation
      being followed rather than the finding being closed. The no-timeout half is not theoretical:
      finding 5 materialised in exactly that shape in M4.5.6.
- [ ] **`kos_cap_narrow` narrows authority but not endpoint rights, so there is no driver-death
      story.** `docs/design-m4-fable-review.md` finding 5's residual API gap, recorded nowhere else.
      **Sharpened 2026-07-31 by building the death gate**, which needed a driver to die and could
      not get one this way: there is NO kernel path that wakes a receiver parked in a receive when
      the last `SIGNAL` holder goes. Only the mirror exists (`recv_holders` -> 0 EPIPEs parked
      SENDERS, `obj_close_protocol`), so a console driver parked in `recv` blocks forever however
      its clients go away, and the sim's console service list's own `n < 0` break is unreachable
      defence rather than a working exit. `tests/integration/check_sim_drvdeath.sh` therefore bounds the driver
      to N served messages instead. So the gap has TWO halves now: root cannot drop `WAIT` while
      keeping the endpoint (the narrow), and a parked receiver has no last-sender wake at all.
      `cap_narrow_authority` (`kernel/syscall/cap.cc`) refuses any handle that does not name the
      authority word with `-KOS_EINVAL`, so "keep the endpoint cap but drop `WAIT`" cannot be
      expressed. That is what defeats the only server-death wake the kernel has: last-receiver-gone
      raises `-KOS_EPIPE` on parked waiters, but root keeps a WAIT-bearing cap on a service endpoint
      so it can hand `SIGNAL` copies to clients, so `recv_holders >= 1` however the server dies and a
      client parked in `kos_call` would block forever. `xmcssc` therefore has to panic on a bring-up
      failure rather than exit, and carries the rule as a comment
      (`system/driver/xmc4800/xmcssc/xmcssc.cc`, `bus_thread`). **Recommendation as recorded**: an
      endpoint-rights narrow is the cheap enabler for a real driver-death story. The generalisation
      is already ABI-free (the handle argument takes any cap); the work is the `recv_holders`
      accounting `obj_close_protocol` does, which the `cap.cc` refusal names as the reason it was
      left out.

## M4.6.2 -- USB CDC console (picopi, pizero2350, teensy41) -- SUPERSEDED by M4.9.1

**`roadmap.md`'s ledger moved this work to M4.9.1 and the RP half is BUILT AND WITNESSED on both
parts.** `system/driver/rp2xxx/rpusb/`, both `service_list_usbcdc.cc` and `user/apps/common/usbcdcwit`
exist; `picopi` and `pizero2350` have each carried CDC bytes on silicon. What genuinely survives
below is the teensy41 seam -- absent by construction, the RT1062 controller needing a cache posture
this tree does not have -- and the design reasoning. Read the planning voice as historical.

**Renumbered from M4.6.1**; the IRQ substrate above took that number, and every reference to
"M4.6.1 (USB CDC)" elsewhere in this file has been repointed here. The dependency is unchanged and
it is not a preference. **The stated REASON was wrong, though, and the design gate corrects it**
(`docs/design-m4.6.2-usb-cdc.md`): the `SETUP` deadline is not what forces the ordering, because
both controllers auto-ACK or NAK in hardware and the software budget is ~2 ms, met by roughly three
orders of magnitude. What actually forces it is reclaim-on-death, a reportable handover failure, and
the two-thread shape -- all three of them M4.6.1's. The sequencing survives; its justification did
not. Building the stack and the foundation it stands on at the same time is still the thing to
avoid.

**A CDC console has a ring, so the panic-path drain is the same question again on two new
controllers.** Branch `tools/panic-ring-probe` carries the instrumentation that answered it for the
PL011: a ring-occupancy query plus two build options, one reporting occupancy at panic entry and one
dropping the drain as a negative control. Both default OFF, so a normal build is unchanged. Rebase
that branch onto whatever tree needs it rather than rewriting it; the negative control is also how
the drain gets mutation-proved without a hand edit that must be reverted. Deliberately not on
`master`, which carries no dev tooling.

**The motivation is that three boards are not self-contained.** `picopi` (GP0), `pizero2350`
(UART1 on GP4/GP5) and `teensy41` (LPUART6, pins 0/1) are the boards whose console needs an
external USB-serial adapter wired to header pins, and `pizero2350` and `teensy41` have no diag
LED wired either. All three parts carry a device-side USB controller, and on `teensy41` it is
the port the board already flashes over, so the board can BE the serial adapter.
**It cost bench time on 2026-07-30, which is the concrete case for it**: `pizero2350` left the USB bus
mid-session -- KickOS has no USB device stack, so nothing on the target answers the host -- and
recovering it needs a physical BOOTSEL press. It cost the rest of that board's session; its
`rootfault` and `rootauth` had already been captured by then, so the debt it created was time, not
witnesses.

**Six more corrections from the design gate, kept here because this section is what a reader hits
first.** The RP2040 and RP2350 USB blocks are the SAME IP, RP2350 a documented superset whose only
software-visible delta is clearing `MAIN_CTRL.PHY_ISO` -- verified register by register, not taken
from the datasheet's own assurance -- so it is ONE backend for two boards. The RT1062 really is a
different programming model, but "ChipIdea" appears nowhere in its RM: it says "EHCI-compatible
core", and device mode is explicitly not EHCI. "+1 backend" undercounts the RT1062, which also drags
in the M7 D-cache. Publishing a USB console BLINDS the pin UART it never touches, so an un-cabled
`picopi` would boot silent -- which is why the gate reverses the driver-death `RECLAIMED` ruling for
this case. The handover probe proves the DRIVER, not the LINK: a UART transmits unlistened, a USB
device does not. And a CDC console does not restore `picotool` recovery, which needs PICOBOOT or a
vendor reset interface.

- [ ] **A CDC-ACM class layer, shared, over TWO device-controller backends.** The class half is
      one implementation: device / config / interface descriptors, the control transfers CDC
      needs (`SET_LINE_CODING`, `SET_CONTROL_LINE_STATE`), two bulk endpoints and one interrupt
      endpoint. The controller half is not shareable across the two families, and that is the
      real cost of adding `teensy41`:
        - **RP2040 / RP2350**: a DPRAM-based USB 1.1 device block. These two appear to be the
          same IP, which would make them the `stm32f411` shape (one backend, two boards).
        - **i.MX RT1062**: a ChipIdea/EHCI-style OTG controller driven by queue heads and
          transfer descriptors, which is a different programming model entirely, not a variant.
      So this is +1 backend for `teensy41`, not +1 stack. **Both claims about the controllers
      are unverified here** and the datasheets are in the local reference set; confirm the RP
      pair really is one block before planning on it.
- [ ] **It is a service, not a port.** A console service kind entry that publishes an endpoint,
      exactly like `k64uartirq`. The handover machinery is transport-agnostic and already carries
      the choreography (create endpoint, publish, grant the window, spawn the unprivileged
      driver, drop root's cap), so nothing in `system/init/` should need to learn about USB.
- [ ] **The panic path reclaims and polls, and this is the part to design rather than discover.**
      `kpanic_enter` already takes the console back from a userspace driver; the USB analogue
      writes into the bulk IN endpoint's DPRAM buffer, marks it available with a length, and
      polls until the controller returns it. **No device-side interrupt is needed, because the
      HOST issues the IN tokens**, so a fault handler can transmit on an already-configured
      device without re-enumerating. That is the DPRAM shape; on the RT1062 the same idea means
      priming a transfer descriptor and polling its status, so the poll is per-backend work and
      needs confirming separately for each. Three constraints follow, and they hold for both:
        - **The spin must be BOUNDED.** With no host holding the port open there are no IN
          tokens and the buffer never comes back, so an unbounded poll hangs a panic on an
          unplugged board instead of reporting and resetting. `KICKOS_POLL_SPIN_MAX` is the
          existing precedent.
        - **A fault before the host finishes configuring has no console at all.** Narrow, and
          no worse than any console needing bring-up, but it means RTT or a pin UART stays
          worthwhile as the early-boot path rather than being redundant.
        - **A suspended device needs resume signalling before it can transmit**, which is the
          one place the fault handler would have to do protocol work rather than a buffer write.
      Expect the same tail loss the UART reclaim already has: `xmc4800-relax` reproducibly
      clips roughly the last 8 bytes the driver had queued (the word pending in `TBUF0`), and
      the USB analogue is a buffer the driver had filled but not yet marked available.
- [ ] **Reboot-to-bootloader takes the USB device away, on all three.** `arch_reboot` hands the
      chip to a bootloader that owns the same port: the RP bootrom re-enumerates as its own USB
      boot device (`2e8a:000f` for the RP2350, witnessed under *`pizero2350`* in
      `docs/reference/boards.md`), and `imxrt1062`'s `bkpt #251` has the MKL02 present HalfKay.
      A USB console goes dark at that call by construction. Correct, and worth stating where the
      reboot seam is documented rather than discovering it on the bench. Note the flip side on
      `teensy41`: that handover is how the board is flashed, so a USB console and the flashing
      path share one connector by design.
- [ ] **Check the idle path against USB liveness before committing to the design.** A device that
      stops answering the host drops off the bus, so whether the tickless idle path keeps the USB
      controller clocked could constrain `arch_idle_wait` on each part. **Do NOT answer it from the
      DAP story.** This item used to say the RP2040 "sleeps when both cores are idle, which is
      already known to gate its debug bus" -- that is a HYPOTHESIS about an open bug whose cause was
      never established (see the DAP item below), not a known fact, so reasoning from it would build
      the USB design on an unproven premise. Answer it from the datasheet, and confirm it by
      measurement on the part.

## M4.9.2 -- the substrate says what it means

Three strands, the first two of which need no board.

### The i.MX RT1062 USB backend, IN this milestone and NOT working yet

A new chip backend, folded in by ruling rather than split off. It is written, build-verified
fleet-wide, and it FAILS ON SILICON at a known point. Do not read it as landed.

- [ ] **THE THREAD STOPS MAKING PROGRESS, AND WHAT STOPS IT IS STILL OPEN.** Read the
      paragraph below for what the clock and window measurements establish, and then the
      one after it for why "the grant" is NOT the settled answer it was written up as.
      (`m492te`, banner `7040afe7`, log `.session/logs/m492te-teensy41-selftest.log`).
      The `[usbclk]` line reads `ccgr6=fc3fc3 pll1=80003040 phyctrl=28200000 phypwd=0
      id=e4a1fa05`. Every clock precondition is met -- CCGR6[CG0] usboh3 gated on, PLL_USB1
      LOCK set and BYPASS clear, the PHY fully powered -- and `id` is exactly `ID_RESET`.
      **The kernel reads USB1 at 0x402E0000 and the controller answers. The unprivileged
      driver thread reads the same address and dies on it**, its breadcrumb stopping at
      `STAGE_PROBE`, which is set immediately before that read. The two accesses differ in
      one thing only, the MPU, so the bus, the clock, the PHY and the reset sequence are all
      out. The controller sequence has never yet been reached, let alone tried.
- [ ] **"DIED AT STAGE X" IS THE DRIVER'S WORD, NOT A MEASUREMENT, AND READING IT AS ONE IS
      WHAT PUT THE GRANT IN THE DOCK.** Root prints `a driver thread never reached its loop`
      after `drv::bring_up` times out waiting for the ready latch; the breadcrumb records
      where the thread HAD GOT TO when that timeout fired. Every stage report in this
      investigation means "was still at stage X when bring_up gave up", never "died there".
      **NEITHER SILENCE IS EVIDENCE, and reading them as evidence was the third wrong
      inference of this chase.** The missing `=== THREAD FAULT ===` line is DESIGNED
      behaviour on this path: `kvprintf_route` does call `kconsole_write` unconditionally,
      but `console_emit` DROPS in `ConsoleState::USER_OWNED` (`kernel/init/console.cc`),
      and `user/src/driver_service.cc` publishes the console BEFORE it spawns the driver
      threads. So from the spawn to the unwind every kernel-console write is dropped, fault
      reports included -- which `rt1062usb.cc` already states in its own header comment.
      `faultsurvive` prints on the other four boards because those runs never publish: the
      STATE differs, not the code path. And a steady dark LED rules out a kernel PANIC only,
      because `kickos_thread_fault_exit` ends at `sched::exit_current(KOS_EXIT_FAULT)` and
      the 3-blink `kfault_terminate` is reached from the panic path alone.
      **So the fault path is back, and in the normal build it is the ONLY survivor.** The
      window between `STAGE_PROBE` and `STAGE_RST_WRITE` is a `set_stage`, one `r32` load, a
      relaxed store and a `set_stage`: no syscall, no loop, nothing that can block or yield.
      The thread there can only fault, stall the bus, or still be running, and the last two
      at a single load would take the core with them -- root printed afterwards.
      **The `KICKOS_RTUSB_UNGRANTED_PROBE` control does NOT discriminate, for a second
      reason on top of its `kos::print`:** `USBPHY1` is itself an off-platform AIPS
      peripheral, so the bridge hypothesis below and the MPU hypothesis predict the same
      death. A discriminating control needs an ungranted RAM address, which only the MPU
      can refuse.
- [ ] **THE STANDING HYPOTHESIS IS THE AIPSTZ BRIDGE, NOT THE MPU, AND THE TREE ALREADY HAS
      THE SEAM FOR IT.** `AIPSTZn_OPACR0..4` reset to `0x4444_4444`, which is Supervisor
      Protect SET for every off-platform peripheral, and `MPR` resets so the bridge honours
      the core's real `hprot[1]`. An unprivileged access to a SP=1 slot is terminated at the
      bridge with an error response and never reaches the peripheral. That fits every
      measurement: privileged kernel read answers, unprivileged read does not, MPU
      irrelevant. `arch.h` already defines `arch_periph_enable` as "ungate the clock AND
      drop the bus-side supervisor-protect", and `system/driver/mk64f/uart_k64.cc` is the
      working precedent on the same vendor's bridge. `imxrt1062` defines no backend, so it
      links the `-KOS_ENOSYS` default and `rt1062usb.cc` never calls it. The RP boards need
      none, which is why nobody hit this before.
- [ ] **IT FORCES A DESIGN DECISION AND THE ANSWER IS NOT OBVIOUS.** The AIPSTZ unit is a
      16 KiB SLOT, and slot 24 holds USB1, USB2 at +0x200 and USBNC at +0x800 -- exactly the
      three the 512 B window was chosen to exclude. `arch.h` grants a base an entry only
      where the bus gate's granularity is CONTAINED by the block and refuses otherwise. On
      this chip it is not contained, so `docs/design-m4.6.2-usb-cdc.md`'s "narrowest unit
      that is its own" does not survive the RT1062. Either accept a coarse AIPS ceiling and
      say so where the design doc claims otherwise, as `frdmk64f` already does, or keep the
      driver privileged. NOT to be settled by whichever is easier to type.
- [ ] **The instrument is the lesson.** Two flashes with a breadcrumb in the driver's own block
      localised this from "somewhere in a 721-line file" to one register, where the two flashes
      before it produced only silence. The block is the only channel that survives the publish,
      needing no console.

### What the teensy41 bench measured, and the two defects it exposed

- [ ] **`bench` DIES ON teensy41 BEFORE ITS SWITCH AND IRQ SECTIONS.** Both runs end with
      `=== THREAD FAULT === thread 'root' killed`, `CFSR=0x82` (DACCVIOL plus MMARVALID),
      `ADDR=0x20200038`, which is OCRAM2. The app's own header advertises context-switch
      cost and IRQ-entry latency; neither ever prints. Not fallout from the MPU enable fix:
      `imxrt1062` is the one chip that calls `kickos_arm_mpu_fixed_init`, so it enforced
      before and after. So the D-cache figures above cover call/reply ONLY.
- [ ] **A call/reply round-trip is about 24,000 cycles at 396 MHz and nothing explains it.**
      61,124 ns with the cache ON, flat from 16 B to 256 B, so it is per-round-trip overhead
      rather than copy cost: two switches, each with a full MPU reprogram. No other board has
      a recorded call/reply figure to compare against, which is itself the gap.

- [ ] **A DISJOINT-DEVICE CONSOLE SHOULD FALL BACK TO `KERNEL_OWNED` ON DRIVER DEATH, and that
      kernel delta is unimplemented.** `docs/design-m4.6.2-usb-cdc.md` section 6.2 rules it.
      Today the state goes `RECLAIMED` instead, which on a board whose reclaim target is a
      different device from the published console yields a working channel anyway, so the two
      differ in bookkeeping rather than in whether the board can speak. That is why it has
      not bitten. Recorded here rather than in a per-board service list, which neither
      implements nor depends on it.

### What the selftest atomics pass left open

- [ ] **`g_cd_lit_rc` and `g_cd_bad_rc` are PLAIN `long` globals with the worker-to-main shape
      of the 56 cells that were just converted** (`selftest/main.cc` around 2408). Their
      siblings `g_cd_goodspawn`, `g_cd_goodname_ran`, `g_cd_neg_ran`, `g_cd_badname_spawn`
      and `g_cd_badname_ran` are plain `int` in the same pattern, and nothing in the file
      says why these are plain while their neighbours are atomic. Either the semaphore
      rendezvous is what serialises them, in which case ONE line at the declaration stops
      the next sweep re-atomising them, or it is a gap. Deciding it also decides whether the
      other 58 needed to be atomic at all, so it is not a local question.
- [ ] **`g_prw` has two writer functions and nothing defends the accumulate.**
      `periph_reg_write_worker` and `periph_reg_write_held_worker` both write it. They cannot
      overlap today because the arm spawns one, waits, reads, then spawns the other. But the
      `load | seen` accumulate is NOT load-bearing (the second read asserts only on bits the
      second worker sets), so a plain store would pass too, and nothing records why it is an
      accumulate.
- [ ] **The two `unsigned char` accumulators should be `uint8_t`** under the fixed-width rule
      in `docs/reference/style.md`. Left as-is because changing them also touches a cast at
      each site.

### Atomic manipulation goes behind kos helpers, never at a call site

- [ ] **THE RULE, and it is broader than the counters:** a call site never spells a load, a
      store or a memory order. Every atomic access is behind a `kos_` helper whose NAME
      carries its contract. `Atomic<T, Order>` does this for the C++-only fields by putting
      the ordering in the type; the C-facing counter fields still need helpers, which in M5
      became `kos_counter_increment` and `kos_counter_load` over a one-member `kos_counter_t`.
      The TYPE is what enforces the rule there: a one-member struct has no `++`, no `+=`, no
      bare read and no bare write, in either language, so a call site cannot violate it.
- [ ] **The counter helper, 21 sites.** Every increment in the tree is a load-then-store pair
      on a `kos_uart_stats` field, across `uart_c6.cc`, `uart_lx6.cc`, `uart_k64.cc`,
      `uart_sci.cc`, `rpusb.cc`, `rt1062usb.cc` and the sim service lists. Those fields were a
      C-facing atomic macro because `uart.h` is C-facing, so a C++ template cannot reach them: the
      helper must be a free function valid in both languages. M5 kept the helper and dropped the
      macro, the fields being single-writer plain words.
      The name must state the SINGLE-WRITER precondition, because the pair is not atomic and a
      second writer loses an update with no gate able to see it. NAME IT
      `kos_counter_increment`, not `bump`, which is jargon and says nothing, and not
      `kos_atomic_*`, which would claim an atomicity a load-store pair does not have.
      `rt1062usb.cc` has a local `bump`; promote and rename rather than duplicate.
- [ ] **Retire "a C driver" as a justification.** It is no longer a goal, so the comments in
      `byte_ring.h` and `uart.h` that cite it must cite the C consumer APP surface instead.
      This does NOT free the shim: `uart.h` carries the `kos_uart_stats` reply a C app names
      and includes `byte_ring.h`, so both stay C-valid on the surviving reason.

### The fleet-wide witness pass, and whatever it turns up

**Last, because it is the only step that needs boards.** It is where every bench-gated item this file
records comes due at once, and each of them is already written down where it was found. **M4.5.6's
bench sessions closed most of what this list used to hold**; what remains is:

- The three M4.9.1 captures predate the merged tree and want retaking: `usbcdcwit` on `pizero2350`,
  the `picopi` `1..104` and the `teensy41` `1..104`. A witness belongs to a TREE.

- `f411disco`'s `f411spi` stage-4 per-app authority witness -- the LAST of the three, and the only
  board of that set still unavailable.
- M4.5.5's general `MinSizeRel` re-witness pass, for the fault addresses, disassembly offsets, symbol
  sizes, stack-depth observations and timing figures that a `Debug` capture cannot carry forward.
- ~~`f302nucleo`'s fault-reporter root cause~~ CLOSED 2026-08-13: it was the flash command's
  `--reset`, and it never needed a replug.
- ~~Right-sizing `frdmk64f` and `bluepill-c8` so `KICKOS_POOL_ARENA_ASSERT` can go fleet-wide.~~
  CLOSED 2026-08-17: the assert is mandatory fleet-wide. Only `frdmk64f` was a right-sizing
  question; `bluepill-c8`'s deficit was entirely its 8K heap carve. Neither board is
  silicon-witnessed, so both remain a MODEL result until an operator takes the K64F.

**CLOSED by M4.5.6, listed so the ledger is not re-opened by habit:** `rx72m`'s one visit (all three
items, `commit 270b6fa`), `esp32c6-wroom`'s `c6blink`, `pizero2350`'s `rootfault` and `rootauth`,
`xmcssc` as a service (the decision is made and the root-MMIO refusal list is gone), and the
RING-ARM witness -- the prober now exists (`user/apps/common/ringpriv`) and `f302nucleo` took it.
Numbered `..N` because a witness pass is expected to OPEN items as well as close them, and M4.5.6's
sessions did exactly that.

**Why the queue is arranged this way.** M4.6.1 and M4.6.2 are both pure code, so **nothing waits on
bench access**: the sub-milestones that can be finished at a desk go first and the one that cannot
goes last. Bench-gated debt stays recorded and explicitly **NON-BLOCKING** -- the precedent is
M4.5.5, whose `rx72m` visit was booked as debt rather than allowed to hold the region-encoding work
open, and which M4.5.6 then paid. `f411spi` is already WITNESS-READY (it builds from this tree,
declares its mask, parks rather than returns, prints an explicit PASS/FAIL, and its flash tooling is
installed), so the pass needs no preparation beyond the board itself.

## An ordering assertion may not rest on a sleep (2026-08-17)

Opened by ONE `call_infoless_revert` failure, seen once in a parallel `ctest` and not reproduced
by 5 standalone and 3 parallel re-runs. It was neither a kernel bug nor a flake to wait out: two
arms staged their choreography on sleep deadlines, and a sleep orders nothing.

**The mechanism, and it is not sim-specific.** `ktime_on_timer` (`kernel/time/time.cc`) drains
EVERY thread whose deadline has passed in one pass, and `policy_pick_next`
(`kernel/sched/policy_fifo_rr.cc`) then takes the strictly highest-priority runnable thread. So
when one stall outlasts the gap between two deadlines, those two wake in PRIORITY order, not
deadline order. The sim runs the whole guest in one host thread, so any host deschedule does it;
on QEMU a loaded host does it; on silicon a long ISR or a slow tickless path does it.
`mtx_time_unit()` does not save an arm: it grows the unit under STEADY load, but a spike arriving
after the calibration defeats it, and its 30 ms glitch cap is a ceiling no calibration can pass.

**THE RULE, which is the part that generalises: an assertion on the relative ORDER of two logged
characters is sound only when the ordering it depends on is established by a rendezvous -- a
semaphore post, a mutex hand-off, a park -- taken by a thread the consumer OUTRANKS, so the
consumer preempts at the post itself. Never by a sleep.** A sleep-staged order assertion is an
instrument whose failure is indistinguishable from the regression it exists to catch. That is
measured, not feared: deleting the D2 boost in `endpoint_call` makes `call_infoless_revert` fail
on `nth('u', 1) < nth('m', 1)`, the SAME assertion the staging race failed. Where a sleep is
unavoidable, every sleeper must satisfy "higher priority implies the earlier deadline", so that a
coalesced drain reproduces the intended order instead of inverting it.

- [ ] **The `depth > KICKOS_MAX_MUTEXES` bound in `mutex_lock` Pass 2 (`kernel/sync/sync.cc`) is
      unexercised.** Arm 19 now reaches hop two; nothing reaches the bound. A third mutex and a
      fifth worker would do it, and the arm is already at the 4-worker pool ceiling, so this wants
      its own arm rather than an extension of that one.

**Why the remaining arms are safe, so the next reader can re-derive it rather than re-audit.**
Every arm was checked against the inverting shape "a lower-priority worker establishes a
precondition on an EARLIER deadline than the higher-priority worker that consumes it". None of
THAT shape is left, which is narrower than it reads: see the correction under this paragraph.
Three structural facts do most of the work: `ktime_sleep_ns(0)` YIELDS instead of parking,
so a 0-unit stage holds no deadline at all; `wq_pop_highest` pops parked senders by priority and
not by arrival, so "who parked first" never decides who is served; and in the sleep-staged arms
that remain, the highest-priority sleeper always holds the earliest deadline, so coalescing
reproduces the intended order.

**CORRECTED 2026-09-17: `sleep_order` is NOT cleared, and the argument that cleared it answered
only half the question.** This entry used to say the two-sleeper arms with equal priorities were
saved by the ready list being FIFO and the sleep queue deadline-sorted. That is true and it
covers exactly one failure mode, the coalesced-drain inversion above. It says nothing about
whether the two deadlines are in the intended order in the first place, and in this arm they
need not be.

`t_sleep` (`user/apps/common/selftest/main.cc`) spawns the 40 ms sleeper FIRST and the 10 ms
one second, so `log_eq("SL")` at `user/apps/common/selftest/main.cc:447` needs the interval
between the two sleepers reaching `kos_sleep_ns` to stay under 30 ms. That interval is the
second spawn's own cost and not a scheduling artefact: root runs at `KICKOS_PRIO_MIN + 1` and
both sleepers at 10, so each create preempts root at the create itself and the sleeper arms its
deadline before root returns to spawn the next one. A spawn is not free on a slow or loaded
box, and nothing in the arm bounds it. Measured on this branch: `sleep_order` went red 1 time
in 19 solo runs in the slow duration band and 6 times in 17 contended ones.

**NOT REPAIRED HERE, and the reason is a missing observable rather than a missing fix.** A
zero-margin expression of the claim needs the arm to know the deadline the kernel actually
armed for each sleeper, which is kernel-side and does not exist today; adding one is a
different piece of work. Two repairs ARE available without it, both leaving a residual margin
rather than removing it, and the choice between them is open:
- Each sleeper publishes its own pre-syscall clock read, and the arm asserts on the OBSERVED
  deadline order (`read + requested`) instead of on the intended one. The residue is the gap
  between the read and the syscall.
- Both sleepers are staged on a rendezvous and released together, so the gap is a post plus a
  switch instead of a whole spawn. The residue is that post and switch.

## More arms a duration can decide, beyond the ones named above (2026-09-17)

A sweep of every TAP-registering translation unit for the shape the section above describes, a
pass or fail that can turn on one wall-clock span outlasting or undershooting another, found
the arms below on top of the ones that entry already lists. **No milestone is assigned here:
`roadmap.md` is the only file that assigns one.** **None is repaired**, deliberately: the
section above already states what a repair has to look like, and a repair that only moves a
margin is not one. Each was read against its own body before being filed.

`call_timeout_reply`, `cap_reply_release_close`, `cap_reply_slot_reuse`, `dev_window_exclusive`,
`thread_join`, `slice_preempts_every_core`, `threads_reach_every_core`, `migrate_running`,
`amp_far_service`, `amp_inbound_reply`, `amp_far_undisclosed`, `amp_far_deliver_fault`, and
`sleep_order`, corrected above.

- [ ] **`call_timeout_reply` is the sharpest of them, because its failure is the only one that
      does not stay inside the arm.** Its settle sleep must finish INSIDE the caller's call
      deadline, so the recv pops a caller that is still parked. The constants' own comment in
      `user/apps/common/selftest/main.cc` records the cost of a flip: the caller times out
      first, the recv returns the report instead of the request, and the arm leaks its reply
      capability into the census `cap_child_width` reads later in the run. So losing this race
      reddens a DIFFERENT arm, and whoever reads that failure is looking at the wrong place.
      Repair it ahead of the others whatever its flake rate turns out to be.
      **An undershoot is a sub-class here and not unique to this arm**: `cap_reply_release_close`
      and `cap_reply_slot_reuse` each need a caller's short sleep to beat the longer one root
      takes before the plain send that ends the server loop, and `thread_join` needs its second
      join, on an already-exited slot, to come back inside the same bound its first join had to
      exceed.
- [ ] **Two of them HANG rather than flip an assertion, which is the worse failure.**
      `cap_reply_release_close` and `cap_reply_slot_reuse` both end the server's recv loop on a
      plain send from root; lose the race and the server leaves before the caller's call lands,
      the caller parks unserved, and the arm sits in its `wait_n` instead of reporting. A red
      names itself in the TAP stream and a hang consumes the whole gate.
- [ ] **One of them goes VACUOUS rather than red.** `amp_far_deliver_fault`
      (`user/apps/common/selftest/selftest_amp.cc`) runs its reply half only if a bounded poll
      sees the caller parked; a bound that runs out skips that half and the arm still reports
      ok. Its call half does fail loudly, so the arm is half instrument and half hope.
- [ ] **The three SMP arms are bounded POLLS against a placement latency, not sleep races.**
      `slice_preempts_every_core`, `threads_reach_every_core` and `migrate_running`
      (`user/apps/common/selftest/selftest_smp.cc`) each give the scheduler a wall-clock budget
      to place or preempt a crowd and then assert on what was observed inside it. A loaded host
      shortens what fits in the budget, not what the kernel did, and the arm reports the
      scheduler failing to reach a core. `migrate_running` at least says so in its own comment,
      having been written to fail rather than hang.
- [ ] **`thread_slay_timeout` already implements the repair pattern, and it is the template for
      every arm where a span genuinely is the subject.** `hog_window_open()` re-reads the clock
      and refuses to proceed unless twice the timeout still remains in the hog's window, retries
      the staging twice, and SKIPS BY NAME rather than asserting when the window is spent. The
      shape to copy is the whole of it: detect the vacuity, and decline instead of asserting
      through it.

### Three the sweep could not rule, recorded as OPEN and not as cleared

- [ ] **`sem_destroy_quiescent` loses its race into a hang, not a flip.** The arm sleeps to let
      the waiter park before main closes its own capability, and the poster sleeps past that
      close. Nothing establishes either ordering. A lost race does not flip an assertion; it
      leaves the arm blocked in its `wait_n(2)`, which costs the gate rather than naming itself.
      Whether the orderings can be lost in practice was not settled.
- [ ] **The bounded-absence IRQ checks fail in the INVERTED direction, so a slow machine makes
      them pass.** `irq_stale_register`, `irq_mask_coalesce` and `irq_discard` each sleep and
      then read a counter to assert that a service did NOT happen. A service that has merely not
      arrived yet is indistinguishable from one that will never arrive, so host slowness buys a
      green run. That is the opposite failure direction from every arm above and it cannot be
      caught by re-running: the arm is vacuous exactly when the box is loaded, and a flake
      hunt looks for reds. Each already declines the half it cannot witness above one kernel
      core, so the shape is understood; the one-core sleep is what is unbounded.

## Named by a commit that fixed something else, and never filed anywhere

Each of these was stated as a known-and-not-fixed caveat in the message of a commit that landed a
different fix. Filing them so they stop living only in `git log`.

- [ ] **`k64uartirq` RX cannot self-recover from an overrun** (`372e7b4`). `OR` blocks `RDRF`, and
      IRQ 32 (UART0 error) is unclaimed, so recovery happens only on the next TX doorbell. Design
      section 7.7 predicts this but says "TX or RX wake"; there is no RX arm, so it is TX-only.
      Invisible on a TX-heavy console, which is exactly why it will surface on the first RX-heavy
      one.
- [ ] **The linker's own `.gnu.warning` output is ignored as noise** (`16662b2`). The missing
      `__getreent` that made every Xtensa stdio call crash was announced by the linker and read past.
      Recommendation as recorded: a `-Wl,--fatal-warnings` gate, or a CI grep for linker warnings.
      Not added.
- [ ] **`microbit` has roughly 800 B of IRQ-binding pool headroom with no assert behind it**
      (`d4898cd`). A future regression there fails at RUNTIME as a mislabelled skip rather than at
      link, which is the failure shape hardest to read. The `bluepill-c8` sibling was right-sized;
      this one was not, because nothing forced it.
- [ ] **The RX72M GROUPBL0 demux is verified correct against the manual and has NO consumer**
      (`a19e484`, `09f02b1`). `rxsci` deliberately uses only the dedicated TXI/RXI vectors, so the
      group path is unexercised even in QEMU. Correct-by-reading is the weakest evidence class this
      project accepts.
- [ ] **`-KOS_ENOMEM` cannot distinguish a full cap table from an empty object pool**, so the
      message is misleading on every board (`8f47990`, which fixed the `xmc4800`/`mk64f` instance
      and left the string alone deliberately). It cost a mis-diagnosis once already: two boards
      skipped `mutex_deadlock` as `SKIP pool too small` when no pool was ever full.

## rx72m: the stop is FIXED, and what it was hiding is the real finding

`d2804ce` got the first bytes ever onto the RX72M wire and `fb739fc` collapsed `console_write` onto
the shared pump. The board now runs the whole suite, and the cure is attributed by a reverting A/B.
Captures: old `.session/logs/m461-rx-fixed-selftest.log`, `m461-rx-led.log`; new
`m461c-rx-uartirq{,-2,-3}.log` and the reproducing control `m461c-rx-noflush{,-2,-3}.log`.

- [ ] **What is actually observed, and it is narrow.** Both captures end mid-string inside a single
      `emitf`: `m461-rx-fixed-selftest.log` at `ok 51`, `m461-rx-led.log` at `ok`. TAP emits a case's
      verdict line AFTER the case body returns, and in the `1..67` numbering #50 is
      `privileged_spawn_refused` and #51 is `irq_thread_ctx`. So **test 51 ran to completion** and
      the stop is in the delivery of its own result line. Two candidates remain, and they are
      different bugs:
      1. **The TX drain wedges** -- `d2804ce`'s own reading, the consumer wedging on the first
         genuinely-full ring. It fits directly: the drain delivers a few bytes of that datagram and
         then stops, and everything pushed afterwards is stranded in the ring. **This is now the
         only candidate with a mechanism**, and it is unproven.
      2. ~~The producer hangs at test 52.~~ **CHECKED AND REFUTED, before it was acted on.** The
         appeal was that `t_irqdrv` (test 52, `irq_as_event`) does
         `TAP_CHECK(drv >= 0); // spawn failure would hang the ready handshake below` and then
         `kos_sem_wait(g_irqdrv_ready)`, and that the image is arena-starved -- it reports
         `heap 16 KiB available` against `30 KiB` on the same board's kernel-console image, because
         `rxsci`'s bring-up costs three threads and a 1 KiB ring block, while the case wants a 4 KiB
         page plus an 8 KiB driver stack. All of that is true **except the premise**: `TAP_CHECK`
         expands to `tap::fail(...)` followed by `return` (`tests/tap/tap.h`), so a failed spawn
         leaves the case, and the handshake below is never reached. That is exactly what the
         comment on that line is for. The suite would have emitted `not ok 52` on a starved arena,
         not gone silent. **Nothing to fix here**, and the arena-starvation observation stands on
         its own as something to watch when this board's image grows.
- [ ] **The probe for the mechanism, and it must not share a channel with the bug.** `d2804ce`
      found the `mark()` TDR probe was itself generating the storm it was measuring, because a TDR
      write with `TIE` armed IS an interrupt. Use `arch_diag_led_set` (P80/PORT8, which shares
      nothing with SCI6 -- `.session/logs/m461-rx-led.log`), not a UART marker. (`RXSCI_TRACE` and
      its `'P'` push are GONE as of the generic-service rework; the warning stands for any
      replacement probe.) The one measurement that settles it: `Shared::stats` already carries
      `irq_wakes`, `tx_bytes` and `irq_spurious`, readable over the endpoint with `KOS_UART_STATS`.
      Run the reverted control, which now reproduces on demand, and read the three counters at the
      stop. Run the `RXSCI_NO_RX=1` control alongside, as `d2804ce` did, so the reading cannot be
      misattributed to RXI6.
      **A reproducible failing image is the asset this leaves behind**: build the tip with the
      `if (n == 0)` arm removed from `rxsci`'s service loop and the stop returns, every time.
- [ ] **The TX drain loop re-reads `SSR` immediately after a posted `TDR` write** (`rxsci.cc`). The
      same file's `tx_irq_disable` documents, with three manual citations, that an RX I/O write is
      POSTED and that a read-back is mandatory before the next instruction may rely on it -- and
      this loop relies on exactly that, expecting `TDRE` to have gone to 0. If the `SSR` read can
      retire first, the loop pops a second byte and overwrites `TDR` before the first reached `TSR`:
      silent byte loss. Same-peripheral read-after-write is very likely ordered on this bus, so this
      is **unproven** -- but it is the identical hazard class the file already treats as mandatory,
      and the guard is one free read.

## Found during the M4.7 cap-rework pass (2026-08-03)

Filed together because one pass turned them up, not because they share a fix: two
scheduler/teardown defects found root-causing the intermittent `sim_stress` failure, two
capability-plane findings from re-reading the reserved indices and the per-board sizing, and one
consumer-facing build limitation that is NOT M4.7 scope. Each says what it rests on and whether it
was read or measured.

- [ ] **Out-of-tree boards are not supported, and that contradicts a stated principle.**
      `cmake/kickos.cmake:30` derives `KICKOS_BOARDS_DIR` from `<repo>/boards` with
      `get_filename_component` -- no cache variable, no override -- and
      `kickos_load_board_descriptor` (`cmake/kickos.cmake:55-73`) has exactly three outcomes: an
      in-tree `boards/<board>/board.cmake`, or, only where `KICKOS_IN_TREE` is FALSE, the single
      board the installed package was built for, or `FATAL_ERROR`. The provisioning path
      no longer looks for a board directory at all: M4.7.5 deleted the per-board headers, so the
      values come from `boards/<board>/configs/<variant>/defconfig` and the only in-tree search
      left is the chip's own include dir. That does not change the conclusion here, because a
      consumer still cannot supply a board without editing the KickOS tree or carrying a patch
      against a vendored copy, while `docs/book/README.md:26` says porting a CPU is "the
      small arch/chip seam, not a kernel restructure". Shape of the fix: make the boards search
      path a LIST a consumer can extend, and let the board include-dir lookup search the same
      list. NOT M4.7 scope -- recorded so it does not evaporate. Read directly.
- [ ] **The concurrent capability-teardown path is never exercised.**
      `kernel/include/kickos/cap.h:326-328` states that an RR slice expiring in `sched::tick_rr` is
      the only thing that switches a dying thread out at a chunk boundary, and so the only way two
      threads are ever inside `cap_teardown` at once. Instrumented counters put that path at **zero
      hits** across the whole suite, including with the quantum cut to 25 us. Forced (a spin in the
      chunk gap plus every churner made RR) it takes 10402 hits with 7 concurrent sweeps and the
      suite still passes, so the design appears sound and nothing guards it against regression. That
      leaves `g_cap.teardown_depth` and `cap_teardown_active()` (`kernel/syscall/cap.cc:52`, bumped
      `:821`, decremented `:872`)
      and the deferred console-death reclaim that reads it (`kernel/sched/sched.cc:209`) untested by
      anything in-tree. A restructuring that removes the chunked window would DELETE the question
      rather than answer it, which is worth deciding deliberately rather than by side effect.
      **Measured by a subagent with instrumented counters -- the zero-hit figure and the forced-path
      figure are both its numbers, worth re-deriving before acting on them.**
      **NARROWED by `tests/unit/capsweep/`**: the host gate now drives one sweep into another and
      pins `teardown_depth` as a COUNT plus the deferred console reclaim that reads it, so what is
      left of this entry is the ON-TARGET hit count, which is still zero. A restructuring that
      removes the chunked window is the S5 mutant of
      `docs/design-m4.8.2-host-unit-tests.md` section 8.6 and now fails a named arm.
- [ ] **`KOS_CAP_CLOCK` is aliased by default spawn delegation, so today it reserves nothing.** It
      is index 1, held for "a board's well-known clock/time service cap"
      (`system/include/kickos/sys/cap_index.h:35`) -- the provision a future userspace CPU governor
      would name, `AUTH_PSTATE` being its matching authority bit. But default placement puts
      delegated cap `i` at child index `i + 1`, so the FIRST delegated cap lands on index 1:
      `user/include/kickos/sys/abi.h:248-249` states the placement ("under default placement they
      land at child indices 1..cap_count") and `abi.h:256-258` states the consequence outright. A
      parent avoids it only by naming a `cap_dest`, and nothing requires one, so the reserved slot
      is routinely overwritten. Closing the aliasing is a precondition for the provision meaning
      anything: either default placement starts at `KICKOS_CAP_FIRST_DYNAMIC`, or the index stops
      being reserved. Read directly; the placement claim checked in `abi.h` itself.
- [ ] **STALE, WRONG MECHANISM (not just a rotted line number): `mk64f`'s handle budget has no
      recorded derivation.** This described `boards/frdmk64f/configs/base/defconfig` and
      `boards/xmc4800-relax/configs/base/defconfig` each stating `KICKOS_MAX_HANDLES=12` directly.
      Neither defconfig sets any such symbol today, and `cmake/cap_table.cmake` now explicitly
      forbids a board from stating the table's width at all ("A board must NOT state the width...
      Kconfig declares no such symbol, so an attempt to set it is refused by name"): the width is
      summed at CONFIGURE time from the kernel's reserved-index count, the service list's retained
      caps (`RETAINED_CAPS`), and the app's declared peak (`kickos_declare_app_capabilities`),
      each stated by whoever owns the fact. Whatever open question this pointed at -- an unjustified `mk64f` figure --
      needs to be re-derived against that configure-time sum; there is no board-stated constant
      left to cite a line number against.

## Found by the 10-angle review (2026-08-02)

Ten angles ran against the finished branch. What they found splits three ways: fixed in the same
pass, recorded here because it is a DESIGN change rather than a patch, and recorded here because it
is a claim I could not verify either way. Each item says which.

### Left owing by the console-reclaim / cancellation change (2026-08-02)

**SILICON, 2026-08-02 at `20f6d43` (`m461m-*`): the change is witnessed and the list below shrank.**
All five `*_uartirq` service lists ran the whole suite through the userspace driver with their
first-light markers on the wire, so the bring-up paths this change touched are exercised on real
hardware: `xmc4800-relax` `1..78`, `frdmk64f` `1..78`, `esp32c6-wroom` `1..78`, `esp32-wroom`
`1..74`, `rx72m` `1..74`, zero `not ok` anywhere. What is still NOT witnessed is the DEFECT PATH
itself -- no bench run kills a live console driver's service thread, so the deferred reclaim and the
cancellation are proven only by `sim_driver_death` case 3 and its four mutation proofs.

- [ ] **`kos_thread_kill` has NO fleet coverage.** Every refusal assertion -- bad handle, big
      handle, a stranger's thread, spawner-accepts, exited-slot EBADF -- lives in the sim
      `drvdeath` app, so no non-sim board exercises the syscall at all. Adding a `thread_kill`
      selftest arm means raising the whole-suite floor `_tap_arms` AND the clause for the region the
      arm lands in, `_tap_arms_p2` for region 2 or `_tap_arms_p1` for region 1
      (`user/apps/common/selftest/CMakeLists.txt`). Read the current values out of that file, never
      out of this entry. The three are deliberately independent expressions, and the split's totality
      FATAL fails the configure on EVERY board the moment p1 + p2 stops equalling the whole, so a
      half-done edit cannot pass quietly -- but the conditional `math(EXPR ...)` clauses beneath each
      one are per-posture, so an arm gated on `KICKOS_HAVE_MPU` moves a clause and not the base.
- [ ] **The stale-spawner-tag clear in `ThreadPool::alloc` is argued, not gated.** It is what
      stops a reclaimed slot's new occupant inheriting the right to kill the previous occupant's
      orphans. Witnessing it needs a thread-slot reuse plus a surviving orphan whose spawner held
      that slot, and no test stages that. It is the ONE part of the kill gate with no mutation
      proof -- everything else in it is proved RED-then-GREEN.
- [ ] **microbit's user arena is 0-7 bytes from the cliff where `mem_self_grant` stops running.**
      MEASURED at `736f4c1`: adding a bare 8-byte kernel `.bss` array flips that arm from RUN to
      SKIP and reds `microbit_selftest`. This is not about any one change -- it means the NEXT
      kernel `.bss` byte anyone adds is a coin flip on that board, and it already forced the
      console-window design away from two stored words and away from a `Thread* spawner` field
      (40 bytes). Give the arena headroom, or make the arm state its requirement, rather than
      leaving the next byte to discover it.
- [ ] **The sim's `arch_console_reclaim_window` returns the fake PV register block unconditionally.** The
      sim console's wire is host fd 1 and its reclaim is a no-op, so this is a modelling statement
      -- "if a sim console driver held registers, these are the registers" -- not a hardware fact.
      True in the only sense the sim can make it true (that block is the only DEV window the host
      admits), and it is what lets case 3 exist at all, but it is the softest part of the gate.
- [ ] **`SIMCON_WIN_BASES` is a THIRD copy of `SIM_PVREG_BASES`** (`sim.cc`, the selftest, and now
      the sim's console service list), each carrying a "must equal" comment and no check. Following the
      existing precedent rather than fixing it was the right call mid-change; fixing it is owed.

### Found by the adversarial review of the cleanup plan (2026-08-03)

The plan was reviewed before execution; it killed three items and found a live bug the plan
never touched. What was FIXED is in the commit. What was found and NOT fixed:

- [ ] **The console register window is stated 7+ times per chip with no cross-check**, and it is
      not an xmc-only shape: `mk64f` is identical and `chip_mk64f.cc` already documents the
      unenforced invariant ("the two must not drift"). Single-sourcing it through a header WAS impossible
      when this was written, because a `system/init/` service list got exactly one include dir
      (`system/include`) and the driver `REGDIR` is PRIVATE, so a chip's base-address header was
      unreachable. M4.7.5 removed that premise: the headers are public as
      `<kickos/chip_mmap.h>`, the chip include dir is on the path, and the rows name the constant.
      The DEDUPLICATION is therefore done. The idea below stands on its own and is not replaced by
      it, because it answers a different question, drift versus authority: `cap_console_publish` has no owner check at all today, and
      requiring the publisher to hold exactly `arch_console_reclaim_window()` closes the drift on
      every board with machinery that already exists (`caller_holds_mmio_block`). That is a
      feature, not cleanup.
- [ ] **Grant span and reclaim span are not required to be equal**, and a future chip may need
      them different. `console.cc` tests RANGE OVERLAP, not equality, so welding both to one
      constant would assert an invariant the code does not have.
- [ ] **`arch/arm/chip/xmc4800/usic.h` and `regs/usic.h` are two near-duplicate copies of the
      USIC channel-offset table in two namespaces**, and `usic.h` carries its own independent
      `0x40030000` literal while `regs/usic.h` derives from `mmap.h`. `usic_uart.cc` consumes the
      un-derived one. Larger duplication than the console window.
- [ ] **Two comments describe a defect two incompatible ways, so one of them is wrong about the
      code**: `tele_pingpong/main.cc` says a non-last thread exit is currently broken on ARM
      while `sched_exit/main.cc` documents it fixed and `check_sched_exit.sh` gates it. (The
      `rxsci.cc` `RXSCI_LED_TRACE` half of this item is RESOLVED: the macro is deleted. Its only
      writer was the service thread's short-accept branch, which `uart::console_thread` now owns,
      so keeping it would have left a witness nothing sets.)
- [ ] **`usbcdcwit`'s `STALL_MAX = 2000` is 5x off its comment** (each zero-accept sleeps 0.2 ms,
      so the bound is ~400 ms, not ~2000 frames). The comment was corrected; if 2000 frames was
      the intent, the CONSTANT is what needs changing.
- [ ] **About 46 ` -- ` occurrences remain, nearly all inside string literals** (re-counted
      2026-08-16; the 72 this box carried had drifted): linker `ASSERT()`
      diagnostics, and the rest `kprintf`/`kos::print`/TAP text. They are user-visible output,
      not comments, and some are grepped by gates (the sim service list's banner), so they were
      left alone. A separate output-text pass could take them.

### Recorded -- reported by an angle, NOT verified by me

- [ ] **`g_stdout_target` is not cleared when the console driver dies**, so children spawned after
      the death are seated on a dead endpoint. Traced: NOT a defect. `kos_send` to an endpoint with
      `recv_holders == 0` returns `-KOS_EPIPE` immediately (`syscall_ipc.cc:115`) and `_write`
      falls back to `kconsole_write` on any non-positive return (`newlib_stubs.cc:45`), which after
      the reclaim drives the polled UART. Cost is one wasted syscall per write. What IS real and
      unfixed: the kernel's identity reference on that endpoint is never dropped, so the endpoint
      object is pinned for the life of the image.
- [ ] **RX72M is said to have lost plain-vector dispatch**, and **`kos_irq_discard` is said to be a
      no-op on three backends**, and **RXv3 is said to write `IR` on a LEVEL line**. Each is
      plausible and each is silicon-gated; none was reproduced this pass. Check them on the bench
      before acting.

## M4.6.2 USB CDC-ACM: WITNESSED on pizero2350 silicon (2026-08-01)

The driver landed at `9832416` unrun. It now works end to end on an RP2350. Captures
`.session/logs/m462-*`.

- [ ] **Still owed**: the production (non-diag) service list on a pure-stdio app, bulk OUT (host
      to device) which nothing has exercised, and `teensy41`, which is a marked seam and not a
      half-built backend. Note `Shared::configured` does not clear on unplug -- no backend arms a
      disconnect or suspend source -- so a host that vanishes without a later bus reset leaves it
      reading 1. Wiring that needs matching resume handling and is bench-gated, not blind.

## The two sharpest IRQ-driver silicon questions, answered from the manuals (2026-08-01)

Both were flagged as the highest-risk unknowns behind the five unrun `*_uartirq` drivers. Verified
against the TRMs in the local reference set, not against HAL headers or the web.

- [ ] **The RX group-mask path has NO in-tree consumer and has never been exercised, even in QEMU.**
      `rxsci` deliberately does not use `TEI6`/`ERI6` (`rxsci.h:19-25`), which are the only GROUPBL0
      sources in play. So the code just verified as correct-per-manual is also completely unwitnessed.
      Worth a forcing consumer before it is relied on.

## f302nucleo: the defect is MISFRAMED -- the fault reporter is innocent (CLOSED 2026-08-13)

**EVERY BOX BELOW IS CLOSED BY THE SAME ROOT CAUSE, and they stayed unticked under a header that
says CLOSED.** `st-flash --connect-under-reset --reset write` left the core under halting debug with
`DEMCR.VC_HARDERR` armed, so the first HardFault halted at the handler's first instruction instead
of running it. Both surviving candidates below -- a bad vector fetch and LOCKUP during stacking --
are FALSIFIED by instrument (`HFSR` `FORCED` with `VECTTBL` clear, `DHCSR` `S_LOCKUP` clear,
`CFSR=0x10000` with no `STKERR`), and the single-change proof is a gdb write of `DEMCR=0x01000000`
on the same boot with no reset and no reflash. Kept for the instrument lessons: the UART markers
were confounded because every marker that fires runs with `CR1.TXEIE` clear, and a debugger left
armed is a legitimate suspect before the silicon is.

Second bench pass of 2026-08-01, at `ab2a52d`(-dirty), by direct-to-`USART2->TDR` marker
instrumentation on the panic path, uncommitted scratch behind a default-off build option.
Captures: `.session/logs/m461-f302-markers{,2,4}.log`, `m461-f302-reset-series.log`.

**The instrumentation was proved BOTH ways before any negative reading was trusted**, because a
silent marker and a broken emitter look identical on the wire. Control `'0'` is emitted from
privileged thread mode in `kmain` right after `kdiag_led_init()`; control `'1'` from UNPRIVILEGED
thread mode on PSP in the app. Both fire. The unprivileged store is sound by ARMv7-M's default
memory map -- the Peripheral region `0x40000000..0x5FFFFFFF` carries no privilege attribute, and the
PPB at `0xE0000000` is the only architecturally privileged-only window, which is the very asymmetry
`ringpriv/ppb.cc` is built on.

**Result on a CLEAN NRST boot, byte-identical across 5 consecutive resets (349 B each):**

```
0 <complete banner> 1 [faul        then nothing, held for 90 s
```

- [ ] **Separately, a rig artifact is now distinguished from the defect.** `hello`'s clean-NRST
      truncation is a FRONT-END LOSS WINDOW at the ST-Link VCOM around NRST, not firmware: nothing in
      this firmware drops the first N bytes and then works flawlessly forever, and
      `CONTEXT.local.md` already records that on this board you must arm the reader and reset as a
      separate step "or the banner is lost". It does NOT explain the marker image's byte-identical
      349 B stop across 5 resets -- host USB timing does not land on the same byte five times. **Two
      phenomena, previously conflated.** The clean separator, if it is ever worth the bench time,
      is to capture on an FTDI wired to PA2 instead of the probe's own VCOM.

**Do not re-derive the old hypothesis list.** Console-broken, ring-flush, reclaim-hang, vector
routing, stack exhaustion and `KICKOS_POLL_SPIN_MAX` are all now moot for this item: they describe a
panic path that a clean boot never enters. `arch_console_reclaim` on this chip is the empty default
(`arch/common/arch_console_reclaim_default.cc`; only `mk64f`, `xmc4800` and `esp32` define a body),
so it could never have hung -- that candidate died on inspection, before the bench.

## M4.6.1 selftest bench pass -- `irq_claim_gate` and `irq_reclaim` on silicon (2026-08-01)

The first silicon witness of the two IRQ-capability cases M4.6.1 added. Default posture on every
board (**no** service-list selection override -- the existing console, not the unrun `*_uartirq`
drivers), `KICKOS_ENABLE_SELFTEST=ON`, `MinSizeRel`. Captures in `.session/logs/m461-*-selftest*.log`.

| board | arch / enforcement | plan | result | skips |
| --- | --- | --- | --- | --- |
| `xmc4800-relax` | armv7m / PMSAv7 `enforce` | `1..71` | all pass | 1 (`mutex_deadlock`) |
| `rx72m` | RXv3 / RX-MPU `enforce` | `1..71` | all pass | 0 |
| `esp32c6-wroom` | rv32imac / PMP `enforce` | `1..71` | all pass | 0 |
| `esp32-wroom` | Xtensa LX6 / `mpu off` | `1..67` | all pass | 0 |
| `frdmk64f` | armv7m / SYSMPU `enforce` | `1..71` | all pass | 1 (`mutex_deadlock`) |

**ALL FIVE BENCH BOARDS PASS.** `irq_claim_gate` and `irq_reclaim` are witnessed on **four ISAs and
four distinct enforcement backends** (PMSAv7, SYSMPU, RX-MPU, PMP) plus one no-MPU part, so the
capability-shaped IRQ line is no longer emulator-only. The `1..71` / `1..67` split is exactly the
enforcement-versus-not plan count STATE.md predicts, machine-confirming the per-posture arm floor.

- [ ] **Captures stamp `ab2a52d-dirty`.** The tree carried the uncommitted f302 marker
      instrumentation (guarded `OFF`, so the selftest images are functionally unaffected) plus doc
      edits. M4.5.6 wrote the rule "commit before a witness pass" and this pass broke it again. The
      selftest evidence stands -- the instrumentation compiles out and touches no selftest TU -- but
      the stamp cannot prove that on its own, so **re-stamp these four captures against a committed
      tip** when the marker work is reverted.

**Rig lesson, and it cost two captures.** On the ESP boards the flash -> arm-reader -> reset-as-a-
separate-step rule (correct for J-Link and ST-Link, where flash and console share one probe) actively
CORRUPTS the log: a second process opening the port to pulse RTS disturbs the already-armed reader,
and the result is a 160-byte capture that is byte-identical across runs -- the ROM banner, then a
contiguous ~2 KB hole swallowing the whole KickOS banner and tests 1..32, then a few lines, then
silence. It reads exactly like a board defect and is not one. `.session/cap_esp.py` exists precisely
for this: it resets and captures on ONE serial handle. With it the same board returned a clean
`1..67`. Both bad captures are kept for comparison (`m461-lx6-selftest.log`,
`m461-lx6-selftest2.log`) alongside the good one (`m461-lx6-selftest3.log`).
**Second, smaller trap:** these serial logs contain NUL bytes, so plain `grep` silently treats them
as binary and prints nothing at all -- not even "Binary file matches". Use `grep -a`, or a skim of
`tail` will disagree with a `grep` of the same file and the `grep` will look authoritative.

## Found verifying the esp32 tree against its TRM (2026-08-01)

The manual arrived and every in-tree register VALUE proved correct. Two citations and one semantic
claim did not. Recorded because the semantic one is the sort that only surfaces as a hang.

- [ ] **Consider the per-SOURCE interrupt mask this port does not use.** TRM 8.3.3: writing an
      INTERNAL interrupt number into a peripheral's map register disables that source for the CPU.
      That is finer than the `INTENABLE` bit the port masks with, which is per-CPU-interrupt and so
      necessarily coarse when sources are OR-ed onto one line. Not adopted, and the reason is sound
      (INTENABLE is core-local and single-cycle; the map write is an APB round-trip inside an ISR).
      It becomes the answer if a second peripheral ever has to share CPU interrupt 13.

## Found while writing the M4.6.2 USB design gate (2026-08-01)

- [ ] **No `arch_console_reclaim` body on `picopi`, `pizero2350` or `teensy41`.** Same gap the UART
      work hit on esp32 and rx72m: a console driver death leaves those boards dark. It bites harder
      for USB, because the panic path there must also deal with a host that may have gone away.
- [ ] **`arch_reboot` is selftest-only on all three USB boards**, and `imxrt1062`'s `bkpt #251`
      HalfKay handover is recorded in-tree as BENCH-UNWITNESSED and not vendor-documented. The
      M4.6.2 section states it as fact; it is not one.
- [ ] **The RP2040 DAP-power-up failure is an OPEN bug with an UNCONFIRMED cause, and the
      workaround recorded for it does not exist.** Two separate corrections to what the notes imply,
      the second confirmed by the maintainer on 2026-08-01.

      First, the flag. Its name is kept in a fenced block below, because naming it in prose would
      fail the very gate that proves the point:

      ```
      KICKOS_RP2040_DEBUG_KEEPALIVE
      ```

      `CONTEXT.local.md` presents it as a default-on build flag that busy-idles core0 so the DAP
      stays live. `git grep` finds ZERO hits across every tracked file, so it is not merely
      disabled, it is absent.

      Second, and this is the part that matters more: **there has never been a working fix, and the
      root cause is not established.** The recorded explanation -- that the RP2040 auto-sleeps when
      both cores WFI and gates the debug bus -- is a HYPOTHESIS that was never confirmed, so it
      should not be repeated as a finding. What is actually known is only the symptom: the first
      flash of a freshly-powered board works, and once KickOS runs, J-Link finds the SW-DP and then
      fails to power up the DAP, after which reflashing needs a power-cycle.

      Consequences. Operationally, a `picopi` running KickOS needs a power-cycle to reflash, full
      stop, and any bench plan that assumes otherwise is wrong. For M4.6.2 it is worse than an
      inconvenience: the design gate asks whether the tickless idle path keeps the USB controller
      clocked, and the honest answer is that this board's idle behaviour is not understood well
      enough to predict it -- the one existing datapoint about idle gating a peripheral is itself
      the unproven hypothesis above. So that question must be settled by measurement on the part,
      not by reasoning from the DAP story. `CONTEXT.local.md` is maintainer-owned and gitignored, so
      the correction there is reported rather than made.

## Found while writing the per-chip UART drivers (2026-07-31)

Four items from the four bench-board drivers. None is a regression from that work; two are
pre-existing defects it FIXED and two are holes it exposed and could not close.

- [ ] **The RX72M group VECTOR is still claimable, so a thread can starve a whole group.**
      `docs/design-m4.6-irq-driver.md` section 9.3 rules that the group vector (110 for GROUPBL0)
      must not be a claimable logical line -- a thread owning "the group" could starve every source
      in it -- but nothing ENFORCES that: `kos_irq_claim(110)` is legal today. The fix is a
      kernel-side line-admissibility hook so a chip can declare a line un-claimable, which is why
      the driver work could not close it (arch and driver code cannot refuse a kernel syscall).
      Until then it is a footgun, not an exploit: only an `AUTH_IRQ` holder can reach it.

## Found while ruling the XMC console seam (2026-07-31)

Both came out of an RM pass for M4.6.1's `TBIEN` question and neither is about the UART. Recorded
here because they are pre-existing isolation facts, not things that pass created.

- [ ] **`FMR.SIOx` lets any window holder pulse ANY of USIC0's service-request nodes with one
      unprivileged store**, so a per-entry seam mask on a USIC interrupt-enable bit is mostly
      blast-radius documentation rather than a boundary. RM V1.3: `FMR` bits 16-21 are `w`,
      classified `U,PV` (`docs/reference/porting.md`), and "writing a 1 to this bit field
      activates the service request output SRx of this USIC channel". `SR[5:0]` are MODULE-scope,
      shared between both channels (RM 18.7, p.18-153), so the target NVIC line may belong to
      another driver or to the kernel. The sibling escape is already WITNESSED: `INPR` is `U,PV`
      and the `inprstorm` capture re-points SR0 from the U0C1 window
      (`user/apps/xmc4800-relax/inprstorm`, `TODO.md` M4.5.6). **Not a regression and not newly
      opened** -- the M4.5.6 verdict on that class was a bounded CPU tax rather than a DoS, on the
      structural `min(fill, drain)` argument. What is owed is honesty in the seam's own comments:
      they should not imply a mask closes what two unprotected registers leave open.
- [ ] **Whether U0C0's `KSCFG.MODEN = 0` gates the whole USIC0 module, which would dark
      `xmcssc`.** RM p.18-153 says "if the module clock is disabled by KSCFG.MODEN = 0, the module
      cannot be accessed", but `KSCFG` is a PER-CHANNEL register at `U,PV`, and the RM text read so
      far does NOT settle whether one channel's `MODEN` gates the module or only its own channel.
      If it gates the module, an unprivileged U0C0 holder can silently kill the SPI service on
      U0C1 -- a cross-channel escape that no seam mask touches, because the store needs no seam.
      **Cheap to settle on the bench**: `pvprobe` already has the shape. Pre-existing, and
      independent of the M4.6.1 console work.

## Found during the M4.5.9 design-tier pass (2026-07-31)

- [ ] **RX72M's ICU reserved block is too small, so Rule 7 cannot refuse an over-broad grant over
      the group registers.** A live grant-admissibility hole, recorded only in
      `docs/design-m4.6-irq-driver.md` section 6.4 and orthogonal to the IRQ work that found it.
      `arch_reserved_blocks` (`arch/rx/chip/rx72m/chip_rx72m.cc:353-371`) reserves
      `{mmap::ICU, 0x400}` with `mmap::ICU = 0x0008_7000` (`platform/rx72m/chip.yaml`), so the
      window is `0x87000..0x873FF`. That covers `IR`/`IER`/`IPR` but **not** `GRPBL0 0x87630`,
      `GENBL0 0x87670`, `GRPAL0 0x87830` or `GENAL0 0x87870`. A privileged over-broad grant
      covering `0x8763x` therefore succeeds today and the Rule-7 predicate has no basis to refuse
      it, which also leaves the SYSMPU/MPU union wrong for anything that does take it. Fix: extend
      the entry to span `0x87000..0x8787F`, size `0x880`. The RX IRQ controller is MPU-GOVERNED
      memory, unlike the ARM PPB, which is why it has to be reserved at all.

## Found during the M4.5.2 stage-2 flip work (2026-07-28/29)

- [ ] **`kos_bus_cfg.cs_index` is accepted and never interpreted.** `k64dspi` drives one hardwired
      GPIO CS (`PTC4`) and `xmcssc` one fixed `SELO0`, so neither `fold()` reads the field, neither
      bounds it, and neither refuses an out-of-range value. Harmless while every driver has one CS
      line, and a trap the moment one has two: the M4.5.2 device slots let a client configure slot 0
      and slot 1 with different `cs_index` values and get the same physical line. `bus-service.md`
      and `bus.h` now say so; a multi-CS driver has to read and bound the field, and that is when
      the `-KOS_EINVAL` refusal the contract wants becomes real.
- [ ] **FOUR in-tree apps grant a DEV window a live board-service driver already holds, so the
      M4.5.2 one-holder-per-window check (`domain_for` -> `-KOS_EBUSY`) now refuses their spawn.
      Silicon-only: no in-env gate covers any of them** (all are registered under
      `kickos_add_diagnostic_apps` or a hardware-observable demo, none has a CTest gate), so
      nothing goes red until the next bench run.
      The same gap covers the suite itself: `dev_window_exclusive` and `bus_device_slots` postdate
      every silicon capture, so the case totals stamped in `docs/reference/boards.md` are right for
      their commits and neither new case has ever run on a chip.
      Verified statically on `xmc4800-relax-st -DKICKOS_HAVE_MPU=1`, whose service list resolves to
      `kickos_services_xmc4800relax` (the polled USIC console on U0C0 + `xmcssc` U0C1) -- the `xmcspi` and
      `consoledemo` ELFs both carry `kickos_board_services`, so both drivers are up before `main`.
        - `xmcspi`, `xmccshold`, `pvprobe`, `inprstorm` each grant `U0C1_BASE`/`0x200` =
          `[0x40030200,0x400303FF]`, the exact window the `xmcssc` bus service holds. This is a REAL
          pre-existing conflict, not a false positive: two drivers configuring one USIC channel. The
          four predate `xmcssc` joining the service list (M4.4) and silently became conflicting then.
          Run them against a console-only service list
          (the service-list selection `kickos_services_xmc4800relax_console`, an existing provider) so U0C1
          has no other holder. Note M4.5.6 changed what these four DO without changing this conflict:
          they now reach FDR/BRG/CCR through `arch_periph_reg_write` instead of writing them directly,
          but they still grant the same U0C1 window, and the one-holder check is about the window.
        - `consoledemo`'s scrambler grants `0x40030000`/`0x200` = the exact window the unprivileged
          console driver holds. Here the double grant is the POINT (garble a live console, prove
          `arch_console_reclaim` recovers it), so the check structurally obsoleted the way it was
          staged. **RESOLVED in M4.5.6, and not the way this entry first proposed**: the scrambler is
          now its own app, `conreclaim`, REGISTERED only when the service-list selection knob already resolves
          to `kickos_services_none` -- a kernel-driven console with no DEV holder anywhere, so the
          scrambler is the sole holder. The scramble-test build option no longer exists.
      **The remedy shape is the part worth keeping, because the obvious one is unbuildable.**
      the service-list selection knob is ONE global cache variable, resolved in the root `CMakeLists.txt` before
      any subdirectory is added, so an app's own `CMakeLists` can never set the list it needs -- it can
      only observe the one already chosen. So the encodable form is a REGISTRATION GATE, not an
      override: register the app when the resolved list is compatible, and otherwise say at configure
      time which list is holding the window and stand down. That is exactly what `conreclaim` does (a
      `message(STATUS ...)` naming the resolved list, then `return()`), which is louder than a silent
      skip and cheaper than a `FATAL_ERROR` that would break every unrelated build of the tree. The
      four U0C1 apps remain a per-image build discipline -- the caller passes the console-only list at
      configure time -- and giving them the same registration gate is the open half of this item.
- [ ] **Respawn vs `-KOS_EBUSY` on the device window -- documented, cannot bite today, revisit with
      SMP or a higher-priority supervisor.** `spi_service.h` says `serve_loop` returns on EPIPE so
      root can respawn; a respawn issued while the dying driver still references its domain would now
      earn `-KOS_EBUSY`. Two independent reasons it cannot happen now: (a) `sched::exit_current`
      calls `cap_teardown` (which EPIPE-wakes the parked respawner) and `domain_release` in the SAME
      `IrqLock` critical section, `cap_teardown` first, so a woken supervisor always observes the
      window already free; (b) root runs at `KICKOS_PRIO_MIN + 1` = 2, below every service driver
      (11-12), so on single-core it cannot preempt a driver between `serve_loop` returning and
      `exit_current`. Opens if a supervisor ever outranks a driver, on SMP, or if death is detected
      any other way (watchdog/timeout/a future join) -- then join before respawning, or retry on
      `-KOS_EBUSY`. Note the respawn path is ALREADY broken for an unrelated reason (the IRQ-line
      entry above), so no in-tree caller exercises this yet.
- [~] **`f411spi` cannot run under the flip: its bring-up shim writes MMIO from `main`. ADDRESSED by
      stage 3, silicon-unwitnessed.** The `stm32f411` `arch_periph_enable` backend covers the SPI1
      clock gate and the pinmux encoding covers `PE3`, but `frdmk64f` was the only board on the bench
      for stage 3, so the `f411disco` run is bench debt. Found 2026-07-29 while flipping `f411disco`, and witnessed
      rather than inferred -- the app faults MemManage on the first store of `main`
      (`RCC_AHB1ENR` @ `0x40023830`, `CFSR=0x82`, `MMFAR=0x40023830`), before it ever spawns the
      unprivileged driver that holds the 32 B SPI1 grant. Same shape as `c6blink` and `rxdrv` before
      their windows were reworked: the escalation surfaces (RCC clock-enable, GPIOA/GPIOE mux) are
      deliberately kept out of the driver's window, which is exactly why they need kernel mediation
      instead of a wider grant. NOT a flip blocker -- it is registered under
      `kickos_add_diagnostic_apps`, never a production image, and the stage-2 gate is `selftest` +
      `rootfault`, both green on that board.
      Its loopback arm is also still unwitnessed in the default posture (needs the PA7->PA6 jumper),
      so the chip's peripheral-window proof stays open either way.

## Found during the M4.5.2 review (2026-07-29)

- [ ] **A user-facing test suite does not exist.** The kernel selftest tests the KERNEL through the
      syscall surface and deliberately does not test the user-facing surface, so nothing anywhere
      checks that `printf`, `std::cout`, heap behaviour and libc integration work per board.
      `hello` passing is the entire coverage, and on some boards `hello` has no gate at all: QEMU
      models no `stm32f302`, so `f302nucleo` carries **no CI gate of any kind**
      (`docs/reference/boards.md`). **The two suites cannot merge**, and the `-st` presets are why --
      the kernel suite is provisioned FOR the kernel. `f302nucleo-st` (`cmake/presets/arm.json:137`)
      now runs it with `KICKOS_USER_HEAP_SIZE=0` and `KICKOS_USER_STACK_SIZE=1024`, which is
      precisely the opposite of what a user-API suite has to exercise. Sharp consequence of that
      preset: with the heap carve at zero the `-st` gate on that board no longer exercises the heap
      at all, so a heap regression on a 16 KiB part would go unseen.

- [ ] **`sam3x8e` over-alignment: PARKED on hardware absence, not open.** The chip HAS an MPU on
      silicon (Atmel SAM3X/SAM3A datasheet, Cortex-M3 revision 2.0) but KickOS ships no `mpu.cmake`
      backend for it, so it builds `KICKOS_HAVE_MPU=0` while still inheriting ARM's fallback
      `arch_mpu_min_region()` of 32 (`arch/arm/common/arch_mpu_min_region_default.cc`) -- costing 3,808 bytes
      of measured over-alignment on a part that enforces nothing. It is the third member of the class
      `stm32f103` and `stm32f302` just left, both of which now override to 0 in
      `arch/arm/chip/stm32f103/chip_stm32f103.cc` and `arch/arm/chip/stm32f302/chip_stm32f302.cc`.
      **PARKED by the maintainer for a concrete
      reason: the physical Arduino Due unit is dead** (`docs/reference/boards.md`), so nothing on this
      chip can ever be witnessed -- do not pick it up expecting to validate it. The class itself is
      handled by the region-encoding item under M4.5.5 above, which is where a third encoding mode
      would land.
- [ ] **Cut `bluepill-c8`'s 8 KiB heap carve: the board is predicted to fail `hello`'s second spawn
      by 96 bytes.** This is a **MODEL PREDICTION, not a witness** -- the board has no physical unit
      and can never be flashed. Arena 6,560, minus idle 512 and root 2,048, leaves 4,000 against the
      4,096 that two 2,048-byte stacks need. The cause is the carve rather than the part: 8 K
      `.userheap` (`arch/arm/chip/stm32f103/stm32f103.ld:27`) where `f302nucleo` now takes 2 K, plus the
      board raising ROOT/USER to 2048 over the chip default of 1024
      (`boards/bluepill-c8/configs/base/defconfig:9`, `:11`). Full arithmetic and its
      provenance are already in `docs/reference/boards.md`; the fix is cutting the carve. The
      prediction is worth acting on because the same model called all three `f302nucleo` silicon
      outcomes correctly -- `hello` two threads, `stress` pass, `selftest` spawns refused.
      **The boot-arena link assert cannot catch this**: `arch/common/boot_arena.ld.h` replays the
      idle and root stacks only, never the N user stacks a spawning app needs.
- [ ] **About 270 `path:N` doc citations cannot be verified by any gate, and two of two spot-checks
      had drifted -- NEEDS A CONVENTION DECISION.** `tests/static/check_doc_names.sh` (landed, deliberately
      not wired into CTest) says so itself at `:54-57`: it strips the `:N` and never checks it,
      because nothing in the current spelling says WHAT should be at that line. The failure mode:
      a citation of `user/include/kickos/sys/abi.h:36` for the cpu-clock syscall resolves to a live
      but unrelated `KOS_SYS_IRQ_CLAIM`, where the real line is elsewhere. This is the
      reused-identifier class the project already knows is expensive, and a citation resolving to a
      live but unrelated thing is worse than one that dangles. Both originally-confirmed instances
      sat in `docs/design-m3-clock-select.md` and were deleted by the M4.5.9 trim rather than
      re-anchored, so the class is unchanged and only the two witnesses are gone. **The decision is the spelling**: if a citation carries the expected symbol
      (`arch.h:84 arch_cpu_clock_hz`), the gate can check it in about two lines; until then `:N` is
      decoration. This item scopes that work only -- fixing the ~270 citations belongs to the doc
      audit, and the two instances above are deliberately left as found.
- [ ] **`arch_reboot` should take a MODE, and the compile knob should gate the MODE rather than the
      seam. Owner: M4.6, after 4.5.4.** Decision recorded in full in
      `docs/design-unprivileged-root.md` section 9, under `### The reboot capability`. Today
      `int arch_reboot(void)` (`arch/include/kickos/arch/arch.h:44`) takes no argument and means
      bootloader entry specifically, with two callers -- `kernel/init/console.cc:293` inside
      `bootloader_handover`, and the `KOS_SYS_REBOOT` dispatch arm at
      `kernel/syscall/syscall.cc:390` -- and the whole thing sits behind `KICKOS_ENABLE_SELFTEST`.
      Four parts: a mode argument (at least a normal system reset and bootloader entry); a per-MODE
      `-KOS_ENOSYS` decline instead of a per-function one; the knob narrowed to the bootloader
      mode and renamed to match the sibling `KICKOS_SHUTDOWN_TO_BOOTLOADER` (`CMakeLists.txt:117`)
      rather than spelling one destination two ways (the proposed spelling is in that design
      section); and an authority bit on top of the knob for the bootloader mode alone. What it
      buys: no production in-kernel path can reset the chip today, which costs watchdog recovery, a
      fault-handler reset and a bring-up retry for no security reason, since a normal reset carries
      none of the bootloader risk. What it retires: `KICKOS_SHUTDOWN_TO_BOOTLOADER` becomes a policy
      on one seam instead of a parallel mechanism, and syscall 38 becomes a real production syscall
      taking a mode -- answering `-KOS_ENOSYS` for a mode the chip lacks and `-KOS_EPERM` without
      authority, instead of `-KOS_EINVAL` from a dispatch default arm -- which takes the
      configure-time `FATAL_ERROR` (`CMakeLists.txt:124`) and the `abi.h:62-64` compiled-out-arm
      annotation with it. The symptom that exposed the conflation:
      `arch/arm/chip/imxrt1062/chip_imxrt1062.cc:49` forward-declares `kpanic` inside a
      `KICKOS_ENABLE_SELFTEST` block, only because that chip's `arch_reboot` is a `bkpt` that must
      not resume -- a fundamental function's declaration behind a test flag.

## Found during the M4.5.3 stage-3 work (2026-07-29)

- [ ] **Consider a diagnosis preset carrying `KICKOS_CONSOLE=both`** -- no board preset sets it,
      `-st` included; NO preset in the tree sets it at all, so a bench run has exactly one transport and
      a published console takes that one away from the app. That is how the phantom SPI halt above
      survived, and any future "the service goes quiet" diagnosis over VCOM alone will re-derive the
      same false conclusion. RTT is generic in the kernel, so `both` builds anywhere, but it is only
      readable where a probe can read target RAM -- the J-Link boards (`xmc4800-relax`, `frdmk64f`)
      are where it pays. Against it: flash, and the two 64 KiB boards have the least of it. The
      bench rule that holds regardless is recorded under *Per-board caveats* in
      `docs/reference/boards.md`.

- [ ] **Audit the whole fleet for the `-Os` clock-gate-then-configure lost write.** On K64F a
      `PIT_MCR = 0` store raced the `SIM_SCGC6` gate write and was **dropped** at `-Os`, fixed by a
      consumed read-back of the gate register (`127efb5`). The same lost-write pattern plausibly
      affects other chips' gate-then-configure sequences now the whole fleet builds `MinSizeRel`.
      Record while auditing that `(void)r32(...)` does **not** serve as a read-back and that
      `-Werror` correctly rejects it: a `(void)` cast of a volatile lvalue performs no access.
      Ranked inventory from a review, unguarded first: **`mk64f arch_pinmux_set`** -- gates a PORT
      then writes that PORT's `PCR` in the same function, write-once with no self-heal, and PORTD's
      `SCGC5` bit was never set on this chip before this milestone; **guarded since `aa084a9`**, and
      the A/B below measures the store landing either way, so treat it as closed rather than as the
      inventory's leading example; **`xmc4800 ccu4_clock_init`** --
      `CGATCLR0`/`PRCLR0` then `GIDLC`, write-once, and `GIDLC` is the monotonic clock's slice
      enable; **`xmc4800 usic.cc kernel_clock_enable`** -- it has the write/read-back/barrier idiom,
      but placed AFTER the first `KSCFG` write and protecting a different documented pipeline effect,
      so the first write into the newly ungated block is itself unprotected; **`imxrt1062
      gpt_clock_init`** -- `CCGR1` then `GPT1_CR`, partially self-healing, but its `SWR`
      software-reset step could silently no-op; the **STM32 family**
      `tim2_clock_init`/`usart2_init`/`arch_diag_led_init`/`arch_pinmux_set`; **`sam3x8e`** (unit
      retired); **`esp32c6 arch_diag_led_init`** (diag only, and that same file already uses an
      explicit `fence` for its `INTMTX` writes). Two are **SAFE for a reason, not by luck**:
      `rp2040`/`rp2350` `unreset()` polls `RESET_DONE`, a real hardware completion flag and a
      stronger pattern than a read-back; `rx72m` `MSTPCR` has substantial intervening work and uses
      the read-back idiom where its UM requires it. **Second hazard variant, unflagged so far:** a
      read-modify-write on a just-gated block can write back a **corrupted** register if the read
      returns stale data -- worse than a dropped store, which at least leaves the reset value.
      `usart2_init` and `arch_diag_led_init` do `MODER` RMWs. **The `k64dspi`/`xmcssc` dropped-mux
      lead is REFUTED -- do not re-run it.** The hypothesis was that a dropped `PCR` write on PTD1
      (SCK) left the pin at its reset mux while the service still reported "up", since nothing checks
      pin state. Measured 2026-07-29 as an A/B one commit wide across `aa084a9`'s read-back: the four
      pin-map rows read their programmed mux **in both arms** -- `PORTD_PCR1`/`PCR2`/`PCR3 = 0x200`
      (ALT2), `PORTC_PCR4 = 0x100` (ALT1) -- and `k64dspi` completes its LAN9252 `BYTE_TEST` round
      trip in both. So the naked store here is not being dropped in practice. **The barrier stays**
      and is not credited with fixing this: it closes a measured hazard for 6 bytes of flash, and the
      window it closes is the tightest instance of the class in the tree -- the disassembly shows
      exactly 1 instruction and 0 intervening bus transactions between the `SCGC5` gate store and the
      `PCR` store, tighter than the PIT failure that proved the class. **The rest of the inventory
      above is untouched** -- those sites are still unaudited; only this one hypothesis died. The
      halt it was chasing does not exist either (see the console-visibility item above).
- [ ] **`k64drv` cannot run under the flip, and is refused BY DESIGN rather than pending a seam.**
      Its PIT window is legitimately granted, but the AIPS `PACR` slot that would have to open for it
      (slot 55) spans the whole 4 KiB block, including the chained ch0+ch1 pair that carries
      `arch_clock_now`, so there is no table entry and `arch_periph_enable` answers `-KOS_EINVAL`.
      This is the opposite case from `f411spi`, which a seam did fix. `k64drv` is a diagnostic app
      (`KICKOS_ENABLE_SELFTEST` only, no CTest gate), so nothing goes red; decide whether it is
      retired or reworked onto a block whose slot is containable. **The third option is gone**: with
      the posture knob deleted in M4.5.6 there is no privileged-root posture to keep it as a
      diagnostic in, so this is now a two-way decision. `arch_periph_reg_write` does not help either
      -- an allowlisted `(base, offset)` still has to sit inside a window the caller holds, and the
      obstacle here is the bus gate's granularity, not the register's.
- [ ] **`f411spi` lost its high-speed slew configuration on `PA5`/`PA6`/`PA7`**, because the pinmux
      encoding field reaches `MODER`/`AFR`/`BSRR` but not `OSPEEDR` or `PUPDR`, so those pins run at
      the reset-default low-speed slew. `BR=/64` is ~1.3 MHz (84 MHz APB2 / 64, arithmetic from the
      tree); that the default slew carries that rate is **engineering judgement, pending a DS9716
      check** -- no line in this tree supports it, unlike the other electrical claims here. Worth
      reopening if a faster `BR` is ever wanted on that bus, which is what would need `OSPEEDR` back
      -- via the encoding, not a root MMIO write.
- [ ] **`cap_console_publish` has no owner check and no once-only guard.** It drops the kernel's
      existing stdout ref and re-points `g_stdout_target` unconditionally, so any caller that clears
      the authority gate silently steals a live console -- and `KOS_SYS_ENDPOINT_CREATE` being
      completely ungated means any thread can mint the endpoint to publish. `AUTH_CONSOLE` is the
      sole thing preventing it, and the guard is wanted independently of that bit: root itself holds
      it for the length of service bring-up.
- [ ] **The CPU/peripheral clock coupling is over-generalised, and the veto should be a notification.
      Owner: M4.6**, and a CPU governor depends on it. `cpu_clock_set` refuses outright while a
      userspace driver owns the console (`kernel/time/clock_select.cc`), because the kernel cannot
      re-derive a baud it no longer owns. That generalises from a biased sample: exactly **two** chips
      implement `arch_periph_clock_hz` and both are coupled (`chip_xmc4800.cc` fPERIPH = fCPU/2;
      `chip_mk64f.cc` `SystemCoreClock` or /BUS_DIV). A chip with an independent peripheral root has
      no backend at all, so the decoupled case has never had to be stated, and the assumption is baked
      into the seam's own contract wording ("retune the core/bus clock") -- on a chip with a dedicated
      CPU PLL there is nothing to refuse. Make the coupling a question asked of the chip, and notify
      affected services instead of vetoing. The console is not the only one: drivers size their
      divisors off `kos_periph_clock_hz` too.
- [ ] **The possession gate has no test distinguishing exact-base from containment.**
      `caller_holds_mmio_block` matches `r.base == base` exactly. Widening it to a containment test
      would pass both mutations `periph_enable_unheld` was checked against, so that regression would
      ship silently. Untestable in-env: the sim's `arch_mpu_region_encodable` is unconditionally
      false, so no DEV region can exist there. The only route is a hosted unit test that fabricates a
      `Thread` plus an `arch_mpu_region` array and calls the predicate directly. The harness now
      exists (`tests/unit/kfixture`); the arm is still owed.

## Needs hardware (bench time, not code)

Every item here is scheduled: it comes due in the M4.6.3..N witness pass, which is deliberately last
so that nothing before it waits on bench access.

- [ ] **`arch_reboot`'s Teensy 4.1 half (`bkpt #251` -> HalfKay) has never run.** RP2350
      (pizero2350, BOOTSEL) is witnessed via `rebootdemo`, and the RP2040 half is now paid too,
      though not via `rebootdemo`: the picopi USB CDC console runs record
      `KICKOS_SHUTDOWN_TO_BOOTLOADER` surviving the fault path (`kickos_terminate` reaches
      `arch_reboot`), so a faulting picopi image returns itself to BOOTSEL/UF2. The Teensy path
      remains the least certain of the three: it is not vendor-documented, and on non-Teensy
      RT1062 hardware the `bkpt` faults instead.
- [ ] **ONE stage-4 per-app authority witness is left, and it is BLOCKED on board access, not on
      work: `f411spi` (F411-disco, PMSAv7).** Three apps were owed -- the others, `c6blink`
      (ESP32-C6, PMP NAPOT) and `rxdrv` (RX72M, RXv3), **were both taken in M4.5.6** at
      `270b6fa`/`270b6fa-dirty`, and `rxdrv` also ran the `kos_periph_enable` possession probe. The
      claim in each case is that a per-app authority declaration carries a board's OWN
      pin muxing on real silicon, and nothing substitutes for the board: the sim can never hold a DEV
      region (`arch_mpu_region_encodable` is unconditionally false there).
      This is the SAME debt the M4.6.3..N ledger carries, not a second one. Both places now use the
      stage-4-authority framing rather than the older "`f411spi` mux write": the mux write is the
      mechanism, the authority declaration is the claim.
      `rx72m`'s coupling to M4.5.5's region re-encoding is discharged: the one visit covered both.
      `f411disco` is a pow2-required backend that M4.5.5 does not move, so its capture is durable
      whenever taken.
      Since M4.5.4 merged without them, this is debt against `master`, not a merge gate.

## M3 -- landed so far (2026-07-20)

Remaining M3 (to finish the milestone) -- gated flow (fable design review -> branch -> silicon):
Silicon target for the handover: the CPU-side-MPU boards (XMC/RX/C6) where per-thread peripheral
isolation is real; K64F is coarse-AIPS (documentation, not enforcement).

Book + exploratory (M3-adjacent, not milestone-gating):
- [ ] **Exploratory spike: microkernel IPC performance** (M3 #4 -> M6). The Mach-era "IPC too slow"
      critique vs the L4/seL4 answer -- (a) fast SYNCHRONOUS IPC (direct switch to the woken
      receiver + register/bounded-copy; KickOS's sem_post already hands the token off and drives an
      immediate switch, so the fastpath shape exists) for control/RPC, and (b) shared-memory + async
      notifications (non-blocking) for throughput -- the M6 cross-core design
      (`docs/design-m7-smp.md`) already uses an SPSC ring + doorbell, exactly that shape.
      Survey the literature, map both to CAP_ENDPOINT (#4) + the M6 rings, recommend the
      control-plane-vs-data-plane IPC strategy + a micro-benchmark. Good deep-research candidate.

## M1 -- clocks (fleet audit 2026-07-09; detail in archived `M1_state.md`)

Every board's timing math is ACCURATE (no ESP32-C6-class constant bug survived the
audit). Remaining work is boards that never raise their PLL, so they run far below
capability and their benchmarks reflect a slow core. Each fix = raise PLL **and**
update `SystemCoreClock` in the same step so the ns<->tick math stays coherent.

- [ ] *(optional perf)* STM32F411 84 -> 96/100 -- deliberate sweet-spot today; only if we
      want the true ceiling. F302 is HW-capped (Nucleo has no HSE crystal);
      K64F/RX72M/F103 already at max; ESP32/RP2040/XMC now at max (silicon-validated).
- [ ] **M10.6.6: ESP32-C6: PLL bring-up 40 -> 160 MHz** (deferred out of M10.5 by the maintainer).
      KickOS sets no C6 clock tree, so every EN reset leaves the CPU on the 40 MHz crystal;
      the kernel reads the rate from PCR at boot, so the bring-up only has to switch the
      source. The C6 AMP judge accepts the crystal rate only and moves with it; the
      wallclock capture is the witness.

## M1 -- hardware validation (batch when units are connected)

- micro:bit / nRF51 -- **QEMU-only; silicon bring-up not planned.** The nRF51 is discontinued
      (no silicon obtainable), so it stays an armv6m QEMU vehicle (`-M microbit`). A real-silicon
      port would also have needed an **RTC-based timer** (the nRF51 M0 has no SysTick).
- [ ] Panic/console review HW-checklist: RP2040 PL011 `TXRIS`-at-rest with FEN=0;
      ESP32 UART FIFO DPORT-vs-AHB alias; RX72M `SCR.TIE`-while-`TDRE` fires TXI. (All
      flagged HW-unverified in-code.)

## M1 -- fleet parity (audit 2026-07-09)

Capability audit across all arch/chip. Fleet is broadly uniform (every arch has a real
console, tickless timer, fault-register dump, inject-driven IRQ path, M2 MPU no-op).
Divergences worth closing for M1, most impactful first:

- [~] **mk64f diag-LED backend ADDED build-only @b5c5665 [DEAD HASH: resolves nowhere -- lost to an earlier history rewrite]** (RED PTB22 active-low) -- code gap
      closed; HW confirm folds into the M2 K64F SYSMPU bring-up (K64F is not an M1 gate, see above).
      **esp32(lx6) DONE** -- GPIO2 (silkscreen D2), validated with `blink` on hardware.
- [ ] *(driver-era, anytime -- NOT M2)* RX `kickos_rx_default_irq` real-peripheral-IRQ demux --
      still a stub (RXv3, a different arch than the C6, so its own work; same concept). Injected
      lines pass selftest but a real peripheral IRQ drops. The C6 `.Lextdev` design is the riscv
      reference pattern. **When the 2nd real device line lands** (fable review finding 5): the
      arch IRQ mask must reach the controller for real lines -- add an `arch_rv_hw_mask` twin (or
      gate `.Lextdev` dispatch on `g_irq_masked` + disable the source), else a tier-1 driver's
      mask-until-ack and the spurious-handler mask silently fail to stop a level source (storm).
      Unreachable today: the C6 console (line 16) is permanently owned + self-gates via INT_ENA.

## M1 -- misc

- [ ] *(dev ergonomics, small)* **debug-in-sleep**: set `DBGMCU` `DBG_SLEEP`/`DBG_STOP` under a
      `KICKOS_DEBUG` gate so SWD survives the idle `WFI` (no connect-under-reset dance to reflash
      a running board). A per-chip one-liner in `arch_init`.

---

## Later -- not M1

**Milestones are keyed to their THEME, not sequence** (audit 2026-07-14). **M2 = MPU /
memory-protection enforcement**, specifically. Work that merely follows M1 is not "M2" unless
it needs the MPU -- the object-pool refactor, worst-case-ISR-latency perf, `sys_cpu_clock_hz`,
and the real-peripheral-IRQ demux are orthogonal (anytime coherence / M3-substrate), tagged
below where they were previously mislabeled.

- **M2 -- MPU enforcement** fan-out: reference pair (RISC-V PMP/NAPOT + XMC v7-M PMSA) ->
  K64F SYSMPU -> RX -> tail; + the arch-independent security model (domains, per-thread
  private stacks, syscall-arg/user-pointer validation, pow2 region placement). See
  `docs/reference/architecture.md` / `docs/m2-readiness.md`.
- **Driver era -- unprivileged MMIO drivers + peripheral-isolation ceiling** (needs the M2
  grant seam; the drivers themselves are anytime coherence). Status in `docs/m2-readiness.md`
  (Driver era subsection) + the fleet peripheral-isolation matrix in
  `docs/reference/architecture.md`.
  - [~] **F411 canonical per-thread PMSA driver (f411spi, SPI1 loopback)** -- BUILT +
        fable-reviewed; **silicon-validation still PENDING** although the disco has been on the
        bench (2026-07-29): the loopback arm needs the PA7->PA6 jumper fitted, and under the flip
        the app faults in its bring-up shim (see the stage-2 findings section). Its PMSA claim is
        no longer the only one -- `xmcspi` proved granted-works/ungranted-faults per thread on PMSA
        silicon in 2026-07 -- so this is now the STM32-family reference rather than a fleet gap.
        `docs/design-spi-driver-stm32f411.md`.
- **[M4] level-trigger tier-1 bindings.** The tier-1 IRQ contract is now latch-and-coalesce
  (a raise on a masked line latches one-deep, redelivered at unmask -- edge-safe, no lost
  pulse). A LEVEL source needs the opposite at rearm: after the driver clears the device, a
  still-asserted line must NOT redeliver a stale latch. The seam is already in place --
  `arch_irq_clear_pending` (added with the coalesce fix) discards the latch; the M4 work is a
  per-binding trigger-type bit in `IrqBinding` (default EDGE) that, for LEVEL sources, makes
  the `irq_wait`/`irq_ack` rearm do `arch_irq_clear_pending(line); arch_irq_unmask(line)` (a
  genuinely-asserted level source re-latches on its own; a deasserted one stays quiet). NOT
  added now: no user/test drives a level binding yet (milestone discipline -- the API bit lands
  with its first consumer). Phantom-defense for level devices lives here too.
- **[M4, lands with bulk-rearm] identity-free coalesced redelivery on the software backends.**
  Today sim/rv32imac/xtensa/rxv3-soft carry a coalesced redelivery through ONE shared cell
  (`pending_irq` / `g_inject_line`) + one physical doorbell, clearing the per-line pending bit
  as it is rung -- so AT MOST ONE `arch_irq_unmask` with a pending redelivery may fire per
  IrqLock region (a second clobbers the first and loses an event). Safe today (register/wait/ack
  each unmask exactly one line per lock section), but a future BULK-rearm path (re-arm many lines
  under one lock) would violate it. Fix when that path lands: stop clearing `g_irq_pending` at
  ring time; have the doorbell dispatcher drain `g_irq_pending & ~g_irq_masked`, looping
  `kickos_isr_irq` over the set bits. Contract stated at the `arch_irq_unmask` decl (arch.h).
- **[anytime coherence -- NOT M2] object-pool mutualisation** -- DONE (step 1). The semaphore
  pool is a generational `SlotPool<T,N>` (slotpool.h); the thread pool is grouped into a
  tailored `ThreadPool` struct (thread.h) -- deliberately **not** SlotPool: thread liveness is
  intrinsic (`state==EXITED`) and its generation bumps at *reclaim* (so a future join-by-handle
  can still resolve a just-exited thread), genuinely different from the sem pool, so forcing
  one pool would be false-DRY. Full unification (a shared handle codec across sems + the M3
  capability store) waits for that genuine second SlotPool-shaped case. (No MPU dependency --
  was mislabeled "M2 handle table"; it's the M3-caps substrate + anytime coherence.)
- **[anytime coherence -- NOT M2] general freeing allocator.** `arch_ram_alloc` is a
  wholesale bump allocator (freed only at reset). Default thread stacks now reclaim via a
  single-size-class intrusive free list in `ThreadPool` (thread.h) -- the special case that needs
  no size metadata (one class == `KICKOS_USER_STACK_SIZE`, link stored in the dead block). A
  GENERAL multi-size-class freeing allocator for `arch_ram_alloc`/`kos_ram_alloc` at large would
  subsume this free list. Until then, only default stacks are reclaimable. The allocator work
  should also reclaim the per-allocation ALIGNMENT RUN-UP, which is dropped on the floor today --
  see the M4.5.1 item above (it is why boot-stack allocation order is load-bearing).
- **[anytime coherence -- NOT M2] user-pointer validation at the syscall boundary.** M2 is MPU
  *enforcement*; validating a user pointer is arch-neutral kernel logic that matters MORE at M1
  (no MPU to contain an OOB access -- see the `user-args-validated-at-boundary` invariant).
  Cheap parts DONE (fable code review): thread name copied into a bounded TCB buffer (fault path
  never derefs/`%s` a user pointer); `clock_now` out-pointer null+8-byte-alignment checked;
  `thread_create_call` stack `base+size` wrap checked; `SlotPool::resolve` rejects a dirty handle top
  byte. Remaining: copy-in the `kos_thread_params` struct via a checked read, and bound-check
  writable out-pointers (`clock_now`) + the `write()` buffer against the caller's granted region
  -- this last part wants the M1 region-ownership model pinned (privileged = whole arena,
  unprivileged = `mem_base`) so it rejects bad pointers without rejecting legit threads.
- **M3 -- capabilities + authenticated grants** (seL4-principled object model), **and
  user-selectable CPU clock / low-power mode** (needs explicit per-chip clock bring-up
  first, from the audit above).
- **[anytime perf -- NOT M2] worst-case ISR latency (shorten interrupt-masked critical
  sections).** Scheduler/switch-path timing, gated on a worst-case-latency probe -- no MPU
  dependency (was mislabeled "M2"). The uniform bench surfaced that under sustained syscall
  load the kernel spends too long masked. Ranked plan (see archived `M1_state.md` section 3.1):
  - [ ] **R1** -- thread a single `now` through switch_to->ktime_rearm->arch_timer_arm +
        arm_slice (kills the 3x arch_clock_now pileup per RR switch; on RX each is a
        nested lock + two 64-bit divides). Cross-arch signature change.
  - [ ] **R3** -- fold the min-delta clock read past arch_timer_arm's idempotency guard
        (so an unchanged-deadline re-arm reads the clock zero times). Combine with R1.
        R3b: add the idempotent-arm guard to xtensa.
  - [ ] **R6** -- xtensa: its cooperative switch runs INLINE under RSIL (masked), unlike
        the 4 other arches that defer the register save/restore to an unmasked handler.
        The one structural outlier; **high risk** (touches windowed-switch atomicity).
  - **Measurement gap (do first):** the current bench measures throughput + *best-case*
    IRQ entry (reporter injects while uncontended), NOT masked-span delay -- so R1/R2/R6
    are not demonstrable with it. Need a worst-case-ISR-latency probe (inject while a
    masked syscall span is in flight) to justify + validate these before landing R1/R6.
  - Note: the earlier **bench self-report starvation is already FIXED** by the
    reporter-as-root/woken-by-workload redesign (not a timer sleep).
- **Console device handover (driver era)** -- userspace UART/console driver takes the
  peripheral as a capability; kernel relinquishes it (`console_tx_deinit`), panic path moves
  to a kernel-retained transport. See `docs/reference/console.md` "Future".
- **[M4.x] Per-thread libc state via real TLS (local-exec).** No per-thread userspace storage
  exists today (newlib `--disable-threads`, threads share one flat image, only the kernel TCB is
  per-thread) -- so `errno` is a shared global, libc `malloc` is not thread-safe (`__malloc_lock`
  is a no-op stub; tracked as its own item in the M4.5.1 kernel-audit section above), and
  `thread_local`/`__thread` silently break. "Fully usable" needs these.

  **CORRECTED BY M5.2.1 PR 8, WHICH MEASURED IT: THESE ARE TWO MECHANISMS AND NOT ONE.** This item
  used to say real TLS fixes `errno` and that a `_REENT`-swap would leave `thread_local` broken.
  Both halves are wrong. `_REENT_THREAD_LOCAL` is off on all three pinned toolchains, `errno` is
  `(*__errno())`, `__errno` returns `_impure_ptr`, and 239 members of libc.a reference that pointer
  without ever calling `__errno()`. So TLS does not touch `errno`, and swapping `_impure_ptr` per
  thread fixes `errno` plus every other reentrant path at once.

  **THE `_impure_ptr` SWAP LANDED IN M5.2.1 PR 9**, on every board but the sim and per-board
  opt-in because it costs one `struct _reent` per thread SLOT out of the app window's heap pad: 512
  bytes on arm-none-eabi, 288 on riscv32-none-elf, 284 on rx-elf. The array is in `.appbss`
  (`user/src/newlib_reent.cc`), `struct Thread` carries the pointer, and `switch_book` plus
  `sched::start` store it into the one word libc reads (`&_impure_ptr`, or what `__getreent`
  returns on Xtensa). It is NAMING AND NOT ISOLATION: the window is granted R/W to every
  unprivileged thread, so a peer can still scribble another thread's `errno`. On by default in the
  `qemu` and `qemu-riscv` base variants, where `errnoprobe` boots it.

  WHAT THIS DOES NOT FIX, and neither does TLS. `_REENT_INIT_PTR` points every thread's
  `_stdin`/`_stdout`/`_stderr` at the ONE shared `__sf[3]`, so stdio buffering stays process-wide
  even where `errno` no longer is.

  **THE MASKED SPAWN WINDOW IS BOUNDED, AND SPLITTING THE LOCK NEEDS THE POOL CHANGED FIRST.**
  The audit calls the interrupt-masked TLS/reent initialisation in `spawn_masked` unbounded.
  It is not, and one of the two bounds landed in this milestone: the TLS copy is at most
  `KICKOS_TLS_STRIDE` minus the ABI bias, which `KICKOS_TLS_FIT_ASSERT` now refuses at LINK
  time; `kickos_reent_init` writes one `struct _reent`, 512 B on arm-none-eabi, 288 on
  riscv32, 284 on rx; the cap loop is bounded by `KICKOS_MAX_SPAWN_GRANTS`. So the window is
  bounded by two link/config constants and nothing an app grows at runtime. The LATENCY
  concern is still real: a board with a full stride of `thread_local` masks on the order of
  2.5 KB of stores.

  **THE REENT TERM IS GONE AS OF M6.2 T5b.2, and NOT by either design priced below.** The kernel
  now primes a slot at its own FIRST SWITCH-IN, so the write happens under the switch's lock and
  never under a spawn's, for every thread rather than for a reused slot alone (the boot seeding
  loop and the pool's `reent_stale[]` went with it). It was moved because a per-process space
  makes a spawn-time write land in the SPAWNER's frame, not to buy latency. What is left in the
  masked spawn window is the TLS copy and the cap loop, so the numbers this section asks for are
  still the ones to take before touching the lock.

  THE OBSTACLE IS NOT THE LOCK, IT IS `ThreadPool::release`. It undoes a claim in exactly two
  cases: an `EXITED` slot, or `i == next - 1`, and its own comment records why that is safe --
  a fresh bump slot is "always the last one UNDER THE SPAWN LOCK". A two-phase publish breaks
  that invariant by construction: a second spawner can claim a slot while the first is
  initialising unmasked, so the first slot is no longer last and `release` either leaves a
  permanent hole (`alloc` only ever revisits EXITED) or un-bumps somebody else's. So the
  change is: give the pool a real free discipline for a claimed-but-unpublished slot, THEN
  split the window, THEN add the reclaim so a spawner slain mid-construction frees the child
  it was building. That third part is what the current single lock buys and must not be lost.

  THE OTHER DESIGN AVOIDS THE POOL AND HITS THE MPU INSTEAD. Have the CHILD seat its own TLS
  and reent on first run: both write the child's own memory, so nothing needs to be masked and
  the spawn lock keeps its transaction. The kernel would pass a trampoline as `entry` and
  carry the real entry and arg in the TCB, which needs no arch change (armv7m already enters
  `entry` directly with lr = kickos_thread_return; only lx6 has a trampoline of its own). It
  does not work as stated: that trampoline runs UNPRIVILEGED for a user thread, so it cannot
  read `user_entry` back out of the TCB. Carrying the pair in the child's own stack above the
  TLS block would work and is the version to price next.

  RECOMMENDATION: neither change should be made without the numbers, because the premise the
  audit gives for making them is wrong. Measure the masked window on a board with a real
  `thread_local` template before touching the spawn path; if it is the few hundred bytes the
  fleet's templates imply, the risk of either design exceeds what it buys.

  **RX HAS NO CI ARM, AND THE BLOCKER IS THE TOOLCHAIN'S DISTRIBUTION.** Every other backend
  has a job in `.github/workflows/ci.yml` (`qemu-arm`, `qemu-riscv`, `qemu-riscv-mpu`,
  `xtensa` as a build gate, `build-boards` for the ARM silicon boards); rx72m appears in
  none, and `build-boards` is arm-toolchain only. The three RX presets DO run in the local fleet
  sweep, which takes every visible configure preset (`tools/sweep_host_gates.sh`, 56 of them on
  2026-08-29), so the gap is CI and not coverage. Adding the job is one composite
  action on the model of `.github/actions/xtensa-toolchain`, which fetches a pinned public
  release asset -- and that is what RX lacks: the installed compiler is Renesas GNURX
  14.2.0.202511 (`rx-elf-gcc (GCC_Build_e13a947a1) 14.2.0.202511-GNURX`), whose distribution
  is not a stable public tarball URL the way the Arm, Espressif and RISCstar ones are. So this
  needs a decision: a mirror to pin, or RX stays locally-verified only and the README says so.

  **M5.2.1 AUDIT: THE MALFORMED-SP BLOCKER IS STILL OPEN, AND THE OBVIOUS FIX DOES NOT WORK.**
  The four refusal paths (armv7m/armv6m `bad_psp`, `kickos_rx_bad_usp`, the rv32imac guard)
  terminate the system rather than the offending thread. Containment itself is available and
  the reasons the code gives for not using it are stale: `arch_ctx_redirect` rebuilds onto
  `ctx.kernel_sp - KICKOS_KERNEL_STACK_SIZE` at the block TOP, so it needs neither a
  trustworthy frame nor a safe SP, and with no block seated it rebuilds at the top of the
  thread's own stack from TCB bounds. A slay-and-resume was written against armv7m and
  MEASURED to fail: a second refusal follows the first, `PSP=0x200007e0 under stack_lo`,
  right after a containment that itself reported correctly. NOT the guard rejecting a
  kernel-block PSP -- `switch.S` already admits `[kernel_sp - SIZE, kernel_sp)` under
  `#if KICKOS_KERNEL_STACKS`. Resuming the SLAIN thread from the fault path is what invites
  the question at all; resuming a DIFFERENT thread avoids it, the dying thread's registers
  being discarded either way. Two further traps found while writing it:
  `kpanic_enter` masks this core's IRQs and never restores them, so it must not run on a path
  that resumes; and `check_pspguard.sh` greps a banner printed BEFORE the outcome, so a
  contained-but-hung image passes on the 20 s `QEMU_TIMEOUT` -- the witness has to assert the
  system went on and exited, not that the banner appeared.

  **PR 9 DOES NOT CLOSE THIS ITEM.** Four pieces of the runtime-consumer scope it was opened for
  are still open, and each is a separate decision rather than more of the same swap:

  - **malloc locking.** `__malloc_lock`/`__malloc_unlock` are still no-ops, so multi-threaded
    `malloc` corrupts the arena. Tracked as its own item in the M4.5.1 section; the reent swap does
    not touch it, `__malloc_sbrk_base` and the bin array living in libc's own statics.
  - **Heap-break serialisation.** `_sbrk` (`user/src/newlib_sbrk.cc`) moves one process-wide break
    with no lock, so two threads growing the heap at once hand out the same page.
  - **C++ exception state.** The unwinder's per-thread state (`__cxa_eh_globals`) is reached
    through `__cxa_get_globals`, which is neither `_impure_ptr` nor a `thread_local` here, so a
    throw crossing a switch is not covered by either mechanism.
  - **Safe reclaim.** A reused slot is re-initialised at the next spawn rather than run through
    `_reclaim_reent`, so the per-reent mprec/asctime scratch a `strtod`/`ctime` caller allocated is
    never returned to the arena. `_reclaim_reent` cannot simply be called on the death path: it
    CLOSES stdio, and every thread's `_stdin`/`_stdout`/`_stderr` point at the one shared
    `__sf[3]`, so reclaiming one thread's state would tear down the whole image's. A reclaim needs
    a newlib-internal-aware sweep of the scratch lists alone, run from a kernel-to-user call on the
    death path PR 7 reworked.

  `thread_local` itself is DONE (PR 8): a block carved off the low end of each thread's own stack,
  with a per-arch thread pointer. Not `TPIDRURW`: M-profile has no such register, so the kernel
  provides `__aeabi_read_tp` (defined in neither libc.a nor libgcc.a) deriving it from SP.
  RISC-V `tp` and Xtensa `THREADPTR` are written by the kernel at resume; RX has neither and falls
  back to emutls. Local-exec model (fully static / no dlopen -> offsets fixed at link). First sibling
  of this family LANDED (M4.3): the `_write` stdout re-probe -- deleted the process-global sticky
  `g_stdout_probe` (per-invocation classify against the calling thread's own cap 0; no per-thread
  storage needed for it).
- **M7 -- multicore (the AMP-versus-shared-kernel question closes PER CLASS; see the OPEN section of the spike).**
  This heading previously read "AMP first on RP2040, SMP-BKL endgame on RP2350" and attributed
  that verdict to the spike. The spike does not contain it and `roadmap.md` says the opposite
  ("not two AMP instances"), so the three records disagreed. Settle it before writing SMP code.
  The deciding measurement is TAKEN: 53 percent of a call/reply round-trip is inside `IrqLock`
  on `esp32c6-wroom` (floor 43 percent), which Amdahl-bounds a two-core big lock at 1.31x --
  see `docs/design-m5-ipc-fastpath.md` 3.0.4. Design spike:
  `docs/design-m7-smp.md` carries the cross-core IPC invariants, the
  per-chip hardware mechanics, the SMP candidate ranking + staged model and the
  SMP-is-per-chip-capability constraint. Candidate
  ranking by the real gate (inter-core atomic + arch-switch maturity): **RP2350 BEST** (M33
  LDREX/STREX enable fine-grained; also 2x Hazard3 -> prove SMP on ARM and RISC-V of one chip),
  **RP2040 big lock FIRST but not capped there** (armv6m has no exclusives, but FreeRTOS V11 ships
  a dual-core RP2040 port carrying TWO locks over two SIO spinlocks and no atomic RMW at all, so
  spinlock COUNT and hold time are the real bounds, not exclusives; what IS unreachable there is
  any lock algorithm needing atomic exchange, so no CLH-style FIFO fairness),
  **ESP32 LX6 last** (an S32C1I CAS is an ISA OPTION this part is not known to configure, and the
  windowed ABI is hardest; unblocked now
  that the fresh-thread-start bug is fixed at 700ec98, still gated on the model proven on M-profile
  first). Staged: (1) big-kernel-lock SMP first (correct on every dual-core, single-core build
  byte-identical), (2) fine-grained only where exclusives exist (RP2350), (3) LX6 after. The spike REVISED the earlier
  "SMP-only, NOT AMP" call below: ARMv6-M (M0+) has no atomics (no LDREX/STREX; the SIO bus is
  non-atomic too), so RP2040 SMP is capped at coarse Big-Kernel-Lock forever -- AMP (two
  core-private kernels + IPC) is the better FIRST step there, and fine-grained lock-free SMP is
  reachable only on RP2350 (M33 exclusives / Hazard3 A-ext). AMP + IPC and the invariant
  refactors are the near-term items; the SMP-BKL plan (one kernel image across cores) stays the
  endgame. Motivation: run the
  dual-core RP2040 (picopi) at 100% under a single KickOS. Biggest architectural axis on the
  roadmap -- it reworks the *foundation*, not a feature: the whole kernel's mutual exclusion is
  `IrqLock == arch_irq_save` ("interrupts off => exclusive"), which is a single-core-only
  guarantee (masking IRQs on one core does nothing to another). Plan:
  - **Step 1 -- Big Kernel Lock.** Redefine `IrqLock` as "disable *local* interrupts + take one
    global spinlock." Centralised, so it's a redefinition of one class, not a 200-site audit;
    every existing critical section keeps working, kernel is SMP-*correct* (coarsely). This
    line used to say "for a 2-core MCU this likely already gives ~2x"; it is MEASURED at
    **1.31x** (`docs/design-m5-ipc-fastpath.md` 3.0.4, 53 percent of a round trip inside the
    lock), and that is before any contention. Per-core run-queues + finer locks are therefore
    not a later *optimisation* but where most of the payoff actually is.
  - **RP2040 specifics:** M0+ has **no atomics** (no LDREX/STREX) -> use the **SIO hardware
    spinlocks** (32 in the SIO block) for the lock; launch core 1 via bootrom/SIO-FIFO
    (`chip_rp2040.cc` already notes the core-1 milestone + the single-core `TIMELR/TIMEHR`
    latch); per-core SysTick + per-core tickless state.
  - **Already seam-ready:** the `KICKOS_*_BARRIER` publish seams (console_tx / rtt) are the
    fence-injection points -- flip to real fences on the SMP build. Keep centralising `IrqLock`,
    structs-over-globals, no ad-hoc masking -> keeps this a redefinition, not a rewrite.
  - Fits the seL4 endgame (seL4 ships a big-lock SMP variant). See `roadmap.md` (M6).
  - **AMP-first on RP2040 (an OPTION, not a spike verdict -- the spike does not contain one).**
    Two core-private `Kernel` instances. The `KICKOS_MULTI_INSTANCE` per-instance seam in
    `instance.h` was described here as "the ~80% substrate", and then as a dead hole that would not
    compile if enabled. **Neither reading holds any more**: the knob is real (`Kconfig`, the root
    `CMakeLists.txt`), the selector is a thread-local in `include/kickos/instance_local.h`,
    `arch/sim/sim.cc` carries the guarded per-instance state, and
    `tests/integration/check_sim_multi_instance.sh` is a registered gate. The two symbols this
    entry named as used-but-undeclared are in no source file in the tree. It is still single-core
    work, and `docs/design-m4.8.2-host-unit-tests.md` still says it is not a prerequisite for a
    Kernel fixture. What IS real substrate is that all shared state already sits in one
    `struct Kernel` behind one accessor, `kernel()`.
    Re-key on SIO CPUID instead of host-TLS. Each core keeps its own run queue + `IrqLock`==PRIMASK, so NO mutual-exclusion
    refactor: AMP de-risks the shared mechanics (core-1 launch, IPC, console arbitration) that
    SMP also needs, and sidesteps the no-atomics problem entirely.
  - **Cross-core IPC -- required for AMP; none exists today** (`Semaphore`/`Mutex` are intra-core
    only). Design in `docs/design-m7-smp.md`: a per-direction SPSC ring in a shared-SRAM
    window (one writer per index + `DMB` ordering -> no lock, no atomics needed on M0+) with the
    SIO 8x32 FIFO used only as a doorbell (write a tag, raise `SIO_IRQ_PROCn`). API = a `Channel`
    (ring + a `Semaphore` in the receiver's kernel) exposed as `KOS_SYS_chan_{open,send,recv}`;
    blocking `recv` parks on the local run queue via `sem_wait`, the peer's SIO ISR drains + wakes
    via the already-ISR-safe `sem_post`. New arch surface is small: `arch_cpu_id`, `arch_dmb`, and
    an `arch_ipc_notify`/`arch_ipc_drain` doorbell pair (so RP2350 SIO-v2 doorbells back the same
    API). The one genuinely-new isolation decision: a fixed `.shared_ipc` region (pow2 for PMSA)
    granted R|W in BOTH cores' MPU sets -- the ONLY cross-core-writable memory; everything else
    stays per-core-private, preserving the per-core-MPU isolation the AMP verdict rests on.
  - **Three single-core invariants to refactor (either path)** -- `IrqLock`==PRIMASK (local-only
    masking; -> BKL or per-core), the single global current-thread/run-queue (per-CPU), and the
    unsynchronised console + boot-on-one-core + single `arch_mpu_apply`. The arch globals
    `g_arch_current`/`g_arch_next` (+ rv32imac `g_isr_depth`/`g_clint_msip`) are the shared
    prerequisite that gates even AMP.

## Pre-M4 perf: caches / flash accelerators (fleet audit 2026-07-22)

Per-chip audit (each vs its RM; see `CONTEXT.local.md` for the local RM set): does the HW have a
software-controllable cache/accelerator, and do we use it? Binary, not "fast enough".

Fleet re-validation follow-ups (from the 2026-07-22 M3-branch gate; see archived `M3_raw_meas.md`):
- [ ] **[post-M4] Port the Thread-Metric benchmark suite to KickOS, into the M8 measurement
      sequence rather than as a standalone comparison.** So we can compare honestly against
      FreeRTOS / Zephyr / ThreadX / PX5 (all run Thread-Metric). Run all contenders on ONE board at
      ONE fixed clock, MPU-on-both-sides where applicable, reporting core/clock/MPU/flags + the
      exact "what is a switch" definition. Published raw-switch figures put KickOS's bracketed
      switch (~66-83 cyc M4/M7) in the ChibiOS band -- but every public number is no-MPU/monolithic,
      so only a like-for-like suite run is defensible. (Zephyr's ~468-524 cyc coop figure looks
      inflated by default-config/methodology, not the kernel -- the suite run would settle it.)
      Direction, keyed to `roadmap.md`'s M8 sequence: port and validate the harness while M8.4
      repairs the instrument's own arithmetic, so the harness is not measured against a broken
      denominator; M8.7 takes its numbers on the same pre-optimisation baseline pass as the rest of
      P0, not before it and not separately; the like-for-like comparison against FreeRTOS / Zephyr
      / ThreadX / PX5 is published only at M8.12, alongside the M8-exit measurement, never at M8.7 --
      an early publish would compare the other kernels against a KickOS number this milestone is
      about to change.
- [ ] **RP2350 v8-M backend advisories A-D (fable review, non-blocking hardening).** From the
      PMSAv8 backend review; none block first enforcement, all are build-robustness / fail-closed
      drift.
      (A) **Fail-closed on non-32-exact regions** in `arch_arm_pmsav8.cc` commit -- mirror rxv3's
          per-region `arch_mpu_region_encodable` check and SKIP (not round) an unencodable region,
          since `__kickos_appdata_start` abuts kernel `_ebss`.
      (B) **Alignment ASSERT** `ASSERT((__kickos_appdata_start & 31) == 0)` in `rp2350.ld` (and add
          the same to `mk64f.ld` -- same latent edge).
      (C) **`DREGION >= kMaxPendRegions` boot check** in `kickos_arm_pmsav8_init` (read
          `MPU_TYPE.DREGION`, do not hard-code 8; fail loud if the budget does not fit).
      (D) **Comment nit** `arch_arm_pmsav8.cc:45-46` / `regs_v8m.h:36-37` -- the PRIVDEFENA-background
          note overstates: a MATCHED region's AP also bounds privileged access.
- [ ] **ESP32-C6 enforce-bench ns-scaling** (measurement-only, not M3). `cyc` counts correct; ns
      ~8x high because `rdcycle` traps on the C6 so the bench samples an MMIO counter whose rate
      differs from `SystemCoreClock`. Also RP2350 bench `irq` reads a bogus 1 cyc (irq-probe not
      wired for the M33). Per-chip bench-instrumentation cleanup, not a kernel bug.
- **No gap (already accelerated), for the record:** STM32F411 ART (ICEN|DCEN|PRFTEN + 2WS,
  `chip_stm32f411.cc:171`); STM32F103/F302 prefetch buffer (M3/F3 have no I/D cache in HW);
  K64F FMC cache+speculation on by reset default (`PFB*CR=0x3004001F`); XMC4800 PMU buffers
  default-on + WS set (`chip_xmc4800.cc:373`); RP2040 XIP cache on by bootrom; ESP32-C6 cache
  fronts external flash only -> irrelevant to KickOS's HP-SRAM execution.
- **RP2350 (deferred M4): XIP cache on by reset + bootrom-invalidated -> NO enable needed** (unlike
  the M7). No Device anti-speculation wrap either -- the M33 isn't speculative and the QMI
  bus-ERRORS (not stalls) on unbacked reads. For the PMSAv8 backend, carry: (1) bound the RX
  region to actual code extent (RLAR arbitrary limit, no pow2 pad) -- the M7 "bounded code"
  lesson; (2) set `XIP_CTRL.NO_UNCACHED_*`/`NO_UNTRANSLATED_*` so mirror-window aliases
  bus-error (saves MPU/SAU regions); (3) MAIR NORMAL-WBWA on the flash region so the cache
  serves hits under enforcement; (4) invalidate-by-address after any future flash program.
  Fold into `docs/design-rp2350-mpu-armv8m.md`.
- Common caveat for ALL the flash caches/buffers: they are NOT coherent across a flash
  program/erase -- any future in-field flash-write/OTA path must invalidate the relevant
  cache/speculation buffer. Not a live risk (KickOS is a fixed flash image today).

## M4.5.x -- foundational tightening

M4.5.x tightens the foundation BEFORE more complexity lands on it. The driver era adds gates,
drivers and controller backends; every one built on a layer that is about to be rewritten is paid
for twice. Less is more: each pass below should end with fewer lines than it started.

## M4.5.9 -- comments and the design tier

Touches nearly every file, so it runs after M4.5.8 merges.

- [ ] **Comment purge.** Keep the fact, cut the chronicle. `--` is a DETECTOR: a comment needing a
      clause chain is already phrased wrong, so rewrite or delete it. Repunctuating to `;` keeps the
      bad sentence and hides the signal.
      **There is no `--` count gate, and there will not be one** (decided 2026-07-31). A zero gate
      is not reachable: a large share of surviving `--` comments are load-bearing, so the only way
      to drive the count down is the repunctuation that destroys the detector. A counting gate would
      then read the tree as clean while every bad sentence survived. The rule stays sweep-on-touch
      plus no new clause chains.
      **Open, and wanted: one ubiquitous mechanism that enforces house style across code, docs AND
      build files.** `clang-format` was already decided against as a gate, and `uncrustify` is not a
      fit either; neither reaches markdown or CMake, and a formatter cannot see the rules that
      actually drift (braced `case` bodies, spelled operators, ASCII, narration). Needs a design
      pass, not another per-rule script.
      Narration is not explanation: the reason a thing is so is one line and stays, the story of
      reaching it is history and git holds it.
      **A comment that turns out to be the only protection for something is a MISSING GATE.** Write
      the test. `virt_rv32.ld` is the model: the `qemu-riscv` gate stops the esp32 assert being copied
      there, not the comment saying so.
- [ ] **Categorize the design tier.** 34 docs, 13,163 lines, against Book 27/7,613 and Reference
      10/7,484 (re-measured 2026-08-16; the earlier 29/10,797 figure had drifted): the tier authoritative for nothing is the largest, and most of it describes landed
      work. Per doc, teaching goes to the Book, the contract to the Reference, and the remainder is
      a short decision list (decisions, why each alternative fails, any falsifier). Nothing left
      means delete it.
      A design doc is neither Book nor Reference. It records decisions for unsettled work, so it
      does not teach and does not restate the contract.
- [ ] **Gate defects, root-caused rather than rewritten.** The four binary-introspection gates go
      GREEN on a broken tool: a pipeline given unexpected input prints nothing, and an
      absence-assertion reads nothing as clean. Verified instances, each a live defect.
      `check_oot_export_mcu.sh` never sets `LC_ALL=C`, so a translated `Machine:` heading fails it
      FALSELY; it reports a broken `readelf` as "did NOT relink", blaming a missing
      `INTERFACE_LINK_DEPENDS`; its `ls *.ld | head -1` silently picks one of several.
      `check_kernel_ctor_placement.sh` runs six bare `nm` pipelines under `#!/bin/sh`, which has no
      `pipefail`, so every exit status is discarded; its own failure diagnostic at line 138 uses
      gawk-only `strtonum`/`and`/`compl`, so a green run never discovers the diagnostic is broken;
      its three address sets are compared as `printf '%x'` STRINGS with nothing asserting the
      formats agree. `check_seam_defaults.sh` guards `UND` but not `ABS`, where `$(($3 + 0))`
      evaluates to 0 and can match section 0; its `TARGET_OBJECTS` split at line 68 is unquoted, so
      a glob character or a space in a build path inventories a different file as an empty one.
- [ ] **Missing gates the purge surfaced.** The rule this milestone runs on is that a comment which
      is the only protection for something is a MISSING GATE, so the sweep was asked to report them
      rather than delete them. Every one below is a real constraint held by prose alone. None was
      removed; each needs a test, and none is a comment problem.
      *Kernel*: the `KICKOS_MIN_STACK_SIZE` per-arch floor is never checked against the deepest
      `exit_current` chain, so a wrong override overflows only on thread exit, on hardware.
      `wq_confirm_resume` requires the lock released BEFORE `wait_result` is read; inlining the read
      under the lock compiles clean and passes on the sim, whose switch is synchronous, and races on
      ARM. `ThreadPool` stack harvest must happen only once the exited thread is provably off-CPU.
      `domain_for` requires `caller_authorized` resolved by the CALLER, never read from
      `sched::current()` inside. `irq_register`'s clear-then-enable order matters only on ARM and RX,
      which are default-masked; sim and riscv would never catch a reorder. `console_tx`'s
      prime-the-pump applies per chip family and a refactor dropping it hangs TX on real
      edge-triggered hardware only.
      *ARM*: `arch_diag_led_init` depends on `uart0_init` having opened the PORTB gate earlier in
      `arch_init`; reordering silently drops the PCR store. `chip_mps2` losing its
      `kickos_arm_pmsav8_init` reference still links and silently writes RASR-shaped values into
      RLAR. `sam3x8e`'s `tc_clock_init` ordering BusFaults a static ctor calling `ktime_now`, and its
      `MOSCXTST` window is a guess pending Due silicon. `imxrt1062`'s MPU-before-cache order
      manifests only as a silicon hang, and that chip has no board or QEMU model. `MODEM_TXCTSE`
      without real CTS wiring makes the polled writer wait forever. `switch.S`'s FP-frame save order
      is covered by `static_assert` for the scalar offsets only, not for `{s16-s31}` before
      `{r4-r11,lr}`.
      *RISC-V and Xtensa*: the C6 all-ones-NAPOT quirk, why the bootstrap entry uses TOR, is not
      reproducible under QEMU. `arch_pinmux_set` writes the GPIO-matrix out-sel before the
      kernel-owned-pin check, so dropping the matrix stage bypasses console and LED pin protection.
      APM denial does not trap the way PMP does, so a bad region hangs instead of faulting.
      *RX*: enabling SCI6 `SCR.TIE` while `SSR.TDRE` is set is EXPECTED to raise TXI6 and has never
      been confirmed on silicon; if wrong, the console TX drain never re-arms after idle. An earlier
      `CMWSTR.STR` readback guard raced at full switch speed and starved the far deadline whenever
      the CPU never idled; nothing stops it being reintroduced.
- [ ] **A registered arm that proves the gates still fail.** Point each gate's tool at `/bin/true`,
      then at a stub emitting a translated heading, then at a truncated capture. Each must exit
      non-zero with a TOOL error, not with the assertion's own diagnostic. Exit codes are read
      unpiped, because `grep -c` exits 1 on zero matches and kills an `&&` chain before its
      diagnostic prints. Without this arm the hand-placed landmark stays a habit of whoever
      remembers, which is what let M4.5.8's eight stacked regressions pass.
      A full Python rewrite of these four was proposed and DECLINED: it moved 762 lines to 585
      while churning `check_riscv_no_smalldata.sh`, already the soundest of the four, and its
      "before M4.6" sequencing rested on M4.6 adding introspection gates, where M4.6.1 and M4.6.2
      add boot and TAP arms riding `tests/lib/gate.sh`, out of that proposal's scope either way.

## Found taking the M4.7.8 payload measurement (2026-08-06)

- [ ] **The call/reply sweep is PRECISE but not ACCURATE, so it cannot accept or reject a change
      of a few percent.** Measured on `xmc4800-relax` silicon, enforcing, one bench variant, 8 B
      round trip. `master` `de2801d` sits at 37997-38152 ns across five builds padded with 0, 4, 20,
      68 and 260 bytes of `.text`, so **layout moves it by 155 ns, 0.4 percent** and the instrument
      is stable per image (the same binary re-flashed is byte-identical). Against that band the
      milestone's own points are 38290, **36047**, 40070 and 39814 at the tip: a spread of 3767 ns
      that layout cannot explain. **The middle point is 2000 ns FASTER than `master` while strictly
      adding code to the path**, which no amount of added work produces, so the number is not a
      per-round-trip cost. Two candidate causes were tested and REFUTED: code layout (the padding
      sweep above) and the deadline cancel that now runs on every wake (removing it made the tip
      slightly WORSE, 40064).
      **The surviving explanation is that the sweep does not measure one thing.** A round trip takes
      the endpoint FASTPATH when a receiver is already parked and the SLOWPATH when the caller parks
      first, and those cost very differently; a small scheduling shift changes the MIX rather than
      the per-path cost, which fits stability per binary, insensitivity to padding, and swings that
      do not track work added. **Fix before trusting it: have the sweep report its fastpath and
      slowpath counts**, so a reading is interpretable instead of an average over an unknown mix.
      Until then the tip's +4.6 percent against `master` is UNEXPLAINED, not established, and the
      argument that the untimed path did not grow is a code reading: two stores at a park, two at an
      unpark, and one comparison against `KOS_TIMEOUT_NONE`.
- [ ] **An `endpoint_call` / `endpoint_recv` kernel signature crossing FOUR arguments was tried and
      REVERTED, because it bought nothing measurable.** Both gained a fifth parameter in M4.7.8 and
      an ARM AAPCS fifth word is passed on the stack, which is visible in the prologues
      (`ldr.w r9, [sp, #64]`) and in a caller-side `str r2, [sp, #104]`. Holding them at four (the
      call taking the lengths already packed by its timed stub, the recv carrying a flag beside a
      `cap_len` that needs only nine bits) removed exactly that traffic and moved the sweep from
      40070 to 40313, i.e. not at all. It also put a bit inside a user-supplied word that userspace
      must never set, which the untimed arm then has to mask defensively. Recorded so the idea is
      not re-derived as an obvious win; revisit only with an instrument that can see it.

## Found taking the M4.7.7 payload measurement (2026-08-06)

- [ ] **`esp32-wroom` reports impossible elapsed times, so its clock read is not trustworthy.**
      The `bench` call/reply sweep at tree `bcb94ff` returned 9 ns and then 0 ns per round trip
      over 20000 calls at the 16 B and 128 B steps, with 1853 ns at 8 B, while its 32/64/256 B
      steps are self-consistent at 75.0 ns per byte and agree with `xmc4800-relax` and
      `frdmk64f` on 9 cycles per byte. So the IPC path is sound on this board and the TIMG0
      read behind `arch_clock_now` (`arch/xtensa/chip/esp32/chip_esp32.cc`) is what returns a
      stale or non-monotonic value: 0 ns across 20000 syscalls is not a slow clock, it is the
      same value twice. Nothing gates it, because no in-env suite reads the clock twice around
      a known interval and asserts the delta grew. A driver-era item: the same read backs every
      timeout a driver takes. Log `.session/logs/m477-esp32-wroom-bench.log`.
- [ ] **Every `kickos_bench_*` helper is a kernel function called directly, so the bench app
      cannot measure anything on a board with an MPU or a PMP.** They are plain calls, not
      syscalls, so they run at the caller's privilege, and each reads kernel `.data`
      (`SystemCoreClock`, the switch accumulators) or a peripheral (`arch_clock_now`). Root has
      not been privileged since `m4.5.1` (`0171b75`), so the first call faults: witnessed as
      `ccu4_ticks` refused at `MMFAR=0x4000c470` on `xmc4800-relax` and `kickos_bench_core_hz`
      refused at `0x1fff0038` on `frdmk64f`, both AFTER the payload sweep, which is why the
      sweep is unaffected. On a board with no unit the reads succeed instead, so the throughput
      and cycle metrics are reachable only there. The `masked_hold` model is already deleted
      rather than repaired, its answer being available from the sweep's slope. What is left is
      a choice for whoever wants the cycle metrics back on an enforcing board: measure inside
      the kernel at boot, or expose the counters through the syscall ABI.

## Found in the M4.7.7 ten-angle review (2026-08-06)

- [ ] **The `bench` app has no execution gate on any board and compile coverage on exactly one
      arch.** `boards/qemu-riscv/configs/bench/defconfig` is the ONLY provisioning in the fleet
      that sets `CONFIG_KICKOS_BENCH=y`, and `user/apps/common/CMakeLists.txt:141` builds the app
      only under that flag, so `qemu-riscv-bench` is the one preset that compiles it. CI then runs
      `ctest -R seam_defaults` against that build dir (`.github/workflows/ci.yml:201`) and nothing
      anywhere registers an `add_test` for the binary, so the app is never EXECUTED by any gate on
      any board. A compile break on armv7m, armv6m, xtensa or RX therefore reaches nobody, and a
      break in the app's own sequencing (its call/reply sweep spawns and joins two peers per step
      against a pool that is 3 slots wide on three boards) reaches nobody at all. Cheapest first
      cut: one more `bench` variant on a board with a machine model plus an `add_test` matching
      the sweep's own output lines, since a bench that prints no measurement is the failure shape.
- [ ] **`cmake/cap_table.cmake` hand-mirrors `KICKOS_THREAD_SLOTS`' `+1` with nothing
      cross-checking it against the C macro.** `kickos_cap_table_resolve` computes
      `math(EXPR _pool "${_threads} + 1")` and comments that it MIRRORS
      `kernel/include/kickos/config/system.h`; if the C side ever changes shape the two silently
      disagree. Bounded today: `_pool` reaches only the configure-time `message(STATUS)` text and
      the `_kickos_cap_slab` arithmetic behind it, never `cap_width.h`, so a divergence yields a
      wrong RAM diagnostic and not a wrong binary. The tree already has the fix shape: the
      structural cap constants (`KCAP_RUN_OFF_POOL`, `KCAP_CHUNK_TARGET`,
      `KICKOS_CAP_FIRST_DYNAMIC`) are declared in `cmake/cap_geometry.cmake` and forwarded to C
      through the header generated from `kernel/include/kickos/config/cap_width.h.in`, so one side
      owns each value. Forwarding the slot count the same way leaves `system.h` deriving the macro
      from the generated header instead of restating `+ 1`.

## M4.8.1 -- every driver gets a class, per the driver-model ruling

The driver era resumes here, and this is the first thing it fixes. `docs/design-m4-driver-model.md`
states the ruling: **"The class is the primitive; the service is a thin thread composed on top of
it. Never the reverse"**, and "A consumer that cannot afford an IPC round-trip links the class and
calls it". In practice the ruling is inverted for **every driver in the tree**, so this is one
fleet-wide conversion and not a per-peripheral fix.

The intended shape, which nothing currently implements: a **struct plus free functions**, a C-like
object holding its own instance state, which the service THREAD instantiates. A consumer that is the
sole user of a bus links the object and calls it, paying no dispatch; the service exists for the
shared case only.

- [ ] **The class leaves are not the class.** `arch/*/chip/*/class/` is real but holds
      register-logic fragments: `dspi_class.h` exposes one function, `dspi_rx_count(base)`. That
      satisfies the Rule 6 seam's "a real leaf and a real consumer each"; it is not a driver.
      Keep the leaves stateless and freestanding as they are -- the class object is a layer above
      them, not a replacement.
- [ ] **The class API must be COMMON per peripheral kind, not per chip.** A client wraps it once and
      stops thinking about the hardware -- that abstraction is the whole point of a kernel plus a
      userspace service layer, and `roadmap.md` already sets it as M4's objective: prove the
      console/UART, gpio, pinmux, clock/power and bus APIs are "genuinely vendor-neutral, not
      accidentally shaped around one vendor". So `spi_transfer(obj, ...)` reads the same against
      XMC USIC and K64F DSPI, and only construction names the chip.
      **Done for SPI, and it was the ruling inverted exactly.** The neutral API used to be
      reachable only over IPC: the old client wrapper was chip-agnostic and took an endpoint
      capability, while the per-chip headers beside it (`xmcssc.h`, `k64dspi.h`) exposed only a
      service start hook, so the only way to get the neutral API was to pay the dispatch. It is now
      `user/include/kickos/driver/spi.h`, the class, with `system/driver/xmc4800/xmcssc/spi_usic.cc`
      and `system/driver/mk64f/k64dspi/spi_dspi.cc` as local engines, `user/lib/spi_proxy/` as the
      proxy over the wire, and `user/include/kickos/sys/spi_service.h` reduced to a transport that
      calls the same class a local consumer links. Still open for the other driver types below.
- [ ] **The unit of commonality is the DRIVER TYPE, and the taxonomy is layered.** One API per
      peripheral kind, not one universal API and not one per chip:
      **SPI**, **I2C**, **UART**, **USB host**, then **per USB device class** (CDC-ACM first, the
      others as they arrive) layered on the host controller rather than beside it, and equally
      **timer**, **PWM** (open: its own type, or a timer with a capture/compare mode), **one-wire**
      and **GPIO**. That list is examples, not an enumeration: **the rule is general to every driver
      type the fleet grows.** A backend that cannot express its hardware in its type's API is the
      signal that the API is shaped around one vendor -- which is the discovery M4 exists to make,
      so treat a genuine misfit as a finding rather than forcing it.
- [ ] **Drivers STACK, and a high-level class must not care whether its bus is local or a service.**
      An accelerometer on SPI is its own class, in its own type; what it needs is a BUS, and that bus
      may be either a local SPI class instance (in-process, no dispatch) or the SPI service over an
      endpoint. So a stacked class is written against the bus type's API and never against a
      concrete backing.
      **This is what the 1:1-serialization requirement actually buys**, and the reason to keep it
      strict: the remote implementation is a PROXY in the ordinary RPC sense, with the identical
      signature to the local object, so substituting one for the other is a build choice and nothing
      above it changes. Let the two drift and every stacked driver has to know which it is talking to.
      The symmetry is also the drift test: the proxy marshals into `kos_call`, and the service thread
      on the other end unmarshals and calls THE SAME local class a local consumer would have linked.
      A call the proxy has and the class does not, or a service that does something the class cannot,
      means the 1:1 property is already broken.
      **DECIDED: the substitution is compile-time, one API with several implementation `.cc` files
      and CMake selecting one.** No function-pointer indirection, no metaprogramming -- the choice is
      known when the image is built, so it costs nothing at runtime and keeps a direct call. For SPI
      that is three implementations of one header: the per-chip local ones (USIC registers, DSPI
      registers) and ONE proxy whose bodies marshal into `kos_call` on the service endpoint. The
      proxy is per BUS TYPE, not per chip, because it speaks the chip-agnostic wire protocol.
      **Two axes select it, and only the first follows the board.** Which chip is a board fact, like
      everything else keyed on `KICKOS_BOARD`. Local-versus-remote is a SYSTEM COMPOSITION fact:
      alone on the bus means local, sharing it with another consumer means remote, and that differs
      per image with the same chip and the same driver source. So it belongs to the CONSUMER TARGET
      rather than to a global macro -- one image may legitimately have one consumer local and another
      remote -- which is the shape the service-list selection knob and the init-provider cache variable already use.
- [ ] **The class must NOT cook. Cooking is the service's job.** Policy in the primitive is what
      makes a primitive unreusable: a class that expands CRLF forces every consumer wanting raw
      bytes to un-cook or thread a flag, and it stops being the plain device. So the UART class
      moves bytes and nothing else; the console SERVICE owns the line discipline.
      **This leaves two cookers, which is correct and not duplication to collapse.** The kernel
      keeps its own in `kernel/init/console.cc` for the pre-handover console and the panic path,
      because a panic cannot call a service. The rule that makes both right: cooking lives with each
      CONSUMER that needs it, never in the device layer. Collapsing them would either put policy
      back in the class or make the panic path depend on IPC.

## Found reconciling the reference documents with the tree (2026-08-25)

- [ ] **RX owes a `pspguard` witness, and the app as it stands cannot give it one.**
      `user/apps/common/pspguard/CMakeLists.txt` returns immediately unless `KICKOS_ARCH` is
      `armv7m` or `armv6m`, and `main.cc` moves SP with ARM asm and `#error`s anywhere else, so the
      PSP-bounds class is machine-checked on two backends of the four that carry it. On rxv3 the
      refusal sites are `.Lsys_bad_usp`, `.Lpendsw_bad_usp` and `kickos_rx_bad_usp`
      (`arch/rx/rxv3/switch.S`); the fourth, `.Lsvc_nokstack`, is structurally unreachable there --
      `thread_create` seats a kernel block on every pool thread and idle is privileged and runs
      `arch_idle_wait` alone, as the site says itself -- so nothing in the tree reaches the REFUSE
      side of the trusted-stack guard on RX. An RX prober needs its own SP-moving arm, and rx72m has
      no emulator, so the witness is a bench step whichever way the app is written.
      **AND THE SP-MOVING ARM ALREADY EXISTS, so what is owed is a bench step and not an app.**
      `user/apps/common/faultsurvive/CMakeLists.txt` builds `faultsurvive_kwrite` and
      `faultsurvive_misalign` on rxv3 as well as rv32imac, and both enter `int #1` with a USP the
      thread chose, which is the entry `.Lsys_bad_usp` guards. What is missing is a run: the
      `add_test` calls for those two are guarded on `rv32imac`, and the board list the file
      registers against holds no RX preset. The arms an RX image can be flashed with today land
      elsewhere -- `faultsurvive_ovf` overflows the stack and the MPU denies the store, and
      `faultsurvive_off` traps through `mvtipl`, a privileged instruction, so it takes the fault
      path where `kickos_fault_frame_trusted` does the checking. So `kickos_rx_bad_usp` is compiled
      into every RX image carrying fault isolation and reached by nothing that runs.

## Found taking the M6.3/M6.4 authoritative witness, 2026-08-29

- [ ] **`tools/sweep_image_gates.sh` SKIPS `qemu-x86_64` BY NAME AND ASSERTS NOTHING ABOUT IT,
      ON A BOX WHERE ITS TEN IMAGE GATES PASS.** `emulator_for()` in that script maps the MPS2
      set and `microbit` to `qemu-system-arm`, `qemu-riscv` to `qemu-system-riscv32`,
      `qemu-riscv64` to `qemu-system-riscv64` and `qemu-arm64` to `qemu-system-aarch64`, and has
      no row for `qemu-x86_64`. An empty answer means "boots natively", which the script accepts
      only for board `sim`, so the x86_64 board falls into the clause below it and is recorded
      `SKIP ... board qemu-x86_64 has no emulator machine; 10 image gate(s) need silicon`. It is
      not silicon-only: `qemu-system-x86_64` is on this box and a plain
      `ctest --preset qemu-x86_64` runs those ten gates green.
      **The tool predicted this failure and caught it in the shape it predicted.** Its header says
      the emulator map is spelled out rather than assumed, and that "a drift between that map and
      `cmake/kickos.cmake` surfaces as a skip this tool did not predict". It surfaced exactly so.
      What it costs: the sweep's DONE sentinel can only be written by DECLARING
      `SWEEP_EXPECT_SKIP=1`, so the declaration mechanism launders a runnable board into an
      expected absence -- an instrument reporting clean over a corpus it never read, which is the
      class this milestone met repeatedly.
      **NOT FIXED HERE, deliberately, and it is a decision rather than a deferral.** Adding the row
      is one line, but whether the sweep's serialised runner drives the OVMF path the way the
      board's own ctest ladder does is unmeasured, and a wrong row produces a red sweep that is an
      instrument fault rather than a finding. Editing a sweep script during the run that sweep is
      the witness for would also invalidate it. Decide the row, then re-take the image sweep.

## External audit of the RV64 backend, 2026-08-29

Five findings against `arch/riscv/rv64imac/aspace_rv64imac.cc`, `arch/riscv/rv64imac/arch_rv64imac.cc`
and `arch/riscv/chip/virt_rv64/startup.S`, held against the RISC-V Privileged specification rather
than against the report. **THREE HOLD AND ARE FIXED. ONE HOLDS AS A PORTABILITY DEFECT WITH NO LIVE
BUG ON THIS BENCH and is fixed anyway. ONE IS A DECISION ALREADY TAKEN and is NOT changed.**
`qemu-riscv64` and `qemu-riscv64-sv48` were 50 of 50 each after the pass and the selftest 133 of
133, one arm more than before; `qemu-arm64` was 40 of 40, the seam having taken nothing. **Those
three ladder totals are DATED to this pass and every count in the items below with them**: the
fleet-wide `whitespace` gate landed later the same day and lifted every board by one, and
`console_reach` lifted the same three by one more -- and `qemu-x86_64`, which this sentence left
out because it was not one of the three the pass measured -- so the registered totals read 52, 52
and 42 now (`ctest -N`, 2026-08-29). **A full run reads 52, 52 and 42, all green.** This paragraph
said `console_reach` was red on the panics still reachable from the console route, which was true
at `517449e5` and stopped being true at `58b43d62` and `d4977780`, the two commits that closed all
four doors; the sentence was not moved with them. The selftest figure DID move and is 134, the plan
line reading `1..134` on all three.

- [ ] **NOCACHE and DEVICE are still accepted where no PTE attribute carries them. RAISED AGAIN,
      NOT CHANGED, AND THIS IS A DECISION.** `docs/design-m6-mmu.md` R2.2 already argues the
      position and the audit's prescribed remedy is wrong as stated. Refusing outright is wrong for
      DEVICE: with Svpbmt absent the attribute is the physical address's PMA, which for an MMIO
      address IS I/O, so a backend refusing DEVICE would refuse a type the platform does deliver.
      Refusing NOCACHE alone has a live caller: `grant_nocache_admissible`
      (`kernel/include/kickos/grant.h`) asks this member on every translating board, so every
      `KOS_MEM_NOCACHE` self-grant and every `kos_task_create` carrying it would answer `-KOS_EPERM`
      here, and `task_handoff_readback` -- which drives both consumers at `KOS_MEM_NOCACHE` with no
      capability skip in front of it -- goes red. The honest answer differs by PHYSICAL ADDRESS and
      the member takes none, which is the gap F8 records. Two things a decision would have to
      settle: whether the answer moves to a per-CHIP seam beside `arch_mpu_nocache_support`, which
      F8's own rule points at and which is a signature move in the milestone whose deliverable is
      the signature diff; and whether the honesty claim in `memtype_known`'s comment, which reasons
      about "this board", may live in an ARCH file every future rv64imac chip compiles.

## Found while witnessing T5, and predating it (2026-08-25)

- [ ] **`fault_dump` HANGS under a non-default service list.** Configured with
      the service-list selection `kickos_services_simuart`, the ctest `fault_dump` gate times out: the
      fault dump prints correctly and the process then never exits. Reproduced on a clean worktree at
      `364aea7a`, so T5 did not cause it. **It is invisible to both sweeps by construction**, nothing
      in `sweep_host_gates.sh` selecting an alternative provider, which is why it survived this long.
      A dump that completes and then fails to terminate is a drain or an exit-path defect rather than
      a reporting one.

## M6.4 x86_64: what the `qemu-x86_64` port owes

Each item is a residual the port left standing on purpose, not a defect it hid. The board's own
caveats and its validation status are in `docs/reference/boards.md`.

- [ ] **The RAM arena is ONE contiguous conventional span, so a machine whose memory straddles
      4 GiB loses the smaller side.** `pick_arena` (`arch/x86/x86_64/entry_x86_64.cc`) merges
      adjacent conventional runs, does not assume the map is sorted, clamps a run straddling the
      1 MiB floor, and then takes the largest single span. At `-m 8G` the largest span is the one
      ABOVE 4 GiB, so 511,373 pages (24.5% of the conventional memory the map names) below the
      boundary go unused. **A multi-run arena is kernel-side work**, not a fix inside this pick:
      `arch_ram_alloc` and the frame pool are both written against one base and one size.
- [ ] **The boot path's stack depth is unbounded and unguarded.** `kickos_x86_64_landed`
      (`arch/x86/x86_64/landed_kernel_x86_64.cc`) moves the kernel onto a 128 KiB image-owned
      static in `.bss`, which is what takes an overrun off firmware's stack and out of the type-7
      Conventional run sitting directly below it. Nothing measures the depth and nothing guards the
      low end. The size matches what UEFI 2.11 section 2.3.4 makes firmware supply, so the switch
      cannot be a regression on that count, which is the whole of the argument standing behind it.
- [ ] **The adopted translation root lives in `EfiBootServicesData`, memory UEFI says the OS may
      reclaim.** This port never reclaims boot-services memory: `conventional_span` takes type 7
      alone, so nothing it hands the allocator can overlap the root, and the regime `arch_init`
      adopted stands for the life of the image. That is an UNSTATED PREMISE rather than an enforced
      one. Nothing asserts it, and the first code that frees boot-services memory pulls the live
      root out from under the map editor.
- [ ] **The i8254 reference timebase is MANDATORY on this board and its rate is HARDCODED.**
      `calibrate` (`arch/x86/x86_64/apic_x86_64.cc`) measures both the local APIC timer and the
      timestamp counter against it, because no CPUID leaf on this processor model reports either
      rate, and there is no second source in the tree. A channel 0 that does not advance is refused
      loudly, so the failure mode is not silence; a channel 0 driven by a different crystal is,
      because `pit_hz` is the literal `1193182` in `arch/x86/chip/q35/chip_q35.cc` and every
      nanosecond conversion is scaled by it.
- [ ] **The xAPIC MMIO window is dereferenced without being mapped or checked, and it is the
      default leg.** `apic_write` and `apic_read` take the MSR path only under `g_x2`; the
      fall-through stores to and loads from `g_mmio + reg`, the address `IA32_APIC_BASE` named.
      The file's own header says why that address is not safe to assume: F8 has it that the adopted
      map covers exactly what the UEFI memory map DEFINES, and the local APIC window is not in it.
      Nothing tests the mapping before the first access, and no gate reaches the leg on a processor
      model that reports x2APIC.
- [ ] **`swapgs` in the trap path keys on the FRAME's cs, so an NMI or a machine check taken in
      either swap window runs on the USER gs base.** `arch/x86/x86_64/trap_x86_64.S` swaps on
      `testb $3` against the pushed cs at entry and against the frame's cs at the resume, which is
      right for an ordinary trap and for a frame some other entry built. It leaves two windows in
      which the pushed cs no longer describes which base is live. **Harmless only because no
      handler dereferences gs**, which is exactly what SMP per-CPU data in the trap path would
      break, so this is a prerequisite of M7 on this arch rather than a standing nit.
- [ ] **TSC invariance is never checked.** `CPUID.80000007H:EDX[8]` is not read anywhere, so
      `clock_now` rests on a rate measured once at `apic_init` and assumed constant. On the
      emulator it is; on a part whose counter varies with the core clock or stops in a deep state,
      every deadline after the change is wrong by the drift and nothing reports it.

## M6.2 closing sweep: two debts the pass could not discharge (2026-08-26)

Both are recorded in `docs/design-m6-mmu.md` beside the obligation they belong to. They are here so
they are known debts rather than forgotten ones.

- [ ] **T7's OWED LATENCY MEASUREMENT, and there is no instrument to take it with.** T7 makes the
      compact-SVC-frame decision wait on an aarch64 round-trip figure: the exception frame is 800
      bytes and a syscall moves about 1.6 KiB with interrupts masked. No such figure exists anywhere.
      The blocker is the rig: `boards/qemu-arm64/configs/` holds a `base` variant alone, so there is
      no `KICKOS_BENCH` image for the board, and `tools/bench/bench-fleet.sh` does not list it.
      **Standing up a bench variant for this arch is a step of its own** and belongs with M8's
      instrument, where the comparison it feeds lives. Until it is taken, no compact-frame decision
      may be argued -- which is what T7 froze.
- [ ] **`qemu-arm64` declares no SERVICE LIST, so F10's real consumer never runs on the one board
      that translates.** F10 makes `drv::bring_up` the gate for the whole allocation ABI and says in
      terms that no selftest arm substitutes for it. It runs on region boards and against host fakes,
      where nothing translates and the same-address rule is vacuous. `task_handoff_readback` is the
      substitution, and it is a good arm that is not the gate. Porting a service list to this board
      is its own step; M6.2 records the gap rather than closing it.

## MMU-era groundwork quick wins (from `docs/design-mmu-era-exploration.md` section 5)

Cheap seam/groundwork changes worth making WHILE the M4-M6 code is written, so the MMU era does not
force a breaking rewrite. Ordered by leverage, as recorded. QW-2 has LANDED (`kaccess_from_user` /
`kaccess_to_user`, `kernel/syscall/syscall_mem.cc`) and is not repeated here.

- [ ] **QW-3. Keep the shared-IPC ring contract PHYSICALLY addressed from day one.** When the IPC
      ring lands (`docs/design-m7-smp.md`), specify that ring control words and slot
      references are offsets or physical addresses, NEVER a pointer valid in one core's space, even
      though on RP2040 (homogeneous, one physical space) a raw pointer would work. It costs nothing
      there and is the exact property a heterogeneous A53/M7 pairing needs. Baking a VA into the
      ring on the homogeneous prototype would silently work until the first MMU peer, then break
      the wire format; getting the invariant into the design text is free, retrofitting it after
      apps depend on the layout is not. The same discipline is what
      `docs/design-m4-fable-review.md` finding 10 wants pulled into the M4 call/reply gate.
      **IT IS LOAD-BEARING FOR M6 NOW, and in the direction of it NOT having landed (2026-08-24).**
      `docs/design-m6-mmu.md` F10 contracts the reserve-then-hand-to-a-task idiom as a same-frame
      handoff at the SAME virtual address, and the reason it must be the same address rather than
      merely the same frames is that nothing guarantees a shared block's CONTENTS are
      position-independent: an address the donor computed can be sitting inside it. This item is
      exactly what would relax that, so landing it buys M6 the freedom to relocate a child's view,
      and leaving it open makes the same-address rule a requirement rather than a convenience.
      Either way it stops being cheap groundwork and becomes a dependency to state.
- [ ] **QW-4. Isolate the pow2/natural-alignment MPU shaping so a page allocator can sit beside the
      bump allocator.** `arch_ram_region_size` / `arch_ram_region_align` encode MPU-descriptor
      geometry into the ALLOCATOR. Flag them as "MPU shaping" belonging behind the same arch family
      switch that would later select frame allocation, with no behavior change, and do not let new
      callers assume "allocation size is always the MPU-rounded size" outside the allocator. The
      pow2 assumption already leaks (`domain_for` dedups on the rounded size) and each new leak is
      another site a frame allocator must reconcile.
- [ ] **QW-5. Confirm the cap/handle layer stays address-space-agnostic, and keep it that way.** A
      do-no-harm review rule, not a change: the per-task handle to per-kernel object-pool model
      (`cap.cc`) is ALREADY MMU-clean, and no cap/endpoint code should start keying on a physical
      address or a region base the way `domain_for` does. Object naming stays purely by
      handle/slot, never by address. Free, since it is the current design, but endpoint IPC is
      exactly the code tempted to stash a shared-buffer physical address in a cap, which would drag
      address-space assumptions into the one layer the MMU rewrite relies on being clean.

### Filed out of S5 by ruling, 2026-09-01

- [ ] **OWED TO WHOEVER SPLITS THE KERNEL LOCK: `prune_empty` clears an entry and frees its table
      in one pass.** On rv64 the peers' invalidation is a rendezvous rather than a broadcast, so
      the entries `prune_empty` itself clears are dropped by the sweep AFTER its frees. The big
      kernel lock is what stops the frame pool reissuing in that window, so it is unreachable
      today and becomes unsafe the moment the lock is split. Closing it needs `prune_empty` split
      into a clear pass and a free pass, with the rendezvous between them. Named rather than
      half-fixed: the ordering around it is already correct for every other free.

- [ ] **`tools/smptrace_decode.py` and `KICKOS_SMP_TRACE` have never been exercised on a real
      stall.** The ring separates three causes of a thread that never runs again and was proven
      only on a passing run, where the right answer is that nothing stalled. Whoever meets the
      next lost wake on any backend should use it FIRST and report whether the three-way verdict
      actually discriminates; if it does not, the fault is in the hook placement rather than in
      the idea, and the hooks are five one-line calls.

- [ ] **Decide what a per-core interrupt controller means for a backend that HAS one.** S5 ruled the
      rv64 software controller image-wide because nothing on that board implements a controller, so
      a line is one logical resource. `docs/design-m7-state-inventory.md` ruling 1 now draws the
      distinction the classification owed: per-core where the cell mirrors a banked register,
      image-wide where it mirrors none. **And the shape S5 had to
      replace is shared by five other backends**: rv32imac, esp32c6, lx6, rxv3 and x86_64 all carry
      a single `g_inject_line` identity plus plain read-modify-write on the mask and pending words.
      That is correct while local interrupt masking is the whole exclusion, which is true of every
      one of them today, and it is exactly what stops being true on the first of them to drive a
      second core. Whichever does it next owes the raised SET and the one-instruction mutations
      rather than a per-core copy.
- [ ] **The cross-core shootdown has no witness on this bench, and that is architecture-mandated
      code rather than a gap to close with a test.** STATE.md already records that removing rv64's
      per-page invalidate leaves every arm green, so the LOCAL invalidate is unwitnessed here; a
      missing REMOTE one is unwitnessed for the same reason and worse, QEMU's TLB model not being
      the machine the ISA describes. What the send has instead is structural: the rendezvous body
      is now reachable and `check_doorbell_generic.sh` asserts it on this arch, where before the
      symbol was dropped for want of a caller. Treated the way the contract treats the data-cache
      seam: correct by construction, recorded as unwitnessed, not described as complete.

- [ ] **The RV64 doorbell's INSTRUCTION-side half has no operation at this board's ISA baseline.**
      The service body carries `SFENCE.VMA`, which orders TRANSLATION and which the ISA gives no
      way for one hart to perform for another. The instruction half is `FENCE.I`, and Zifencei is
      absent from `arch/riscv/chip/virt_rv64/cpu.cmake`'s march string, so it does not assemble.
      Ruled DEFERRED rather than papered over: calling `SFENCE.VMA` sufficient would put an untrue
      statement of contract in the tree. No caller exists yet, the rv64 instruction-side rendezvous
      being unwired and dropped by `--gc-sections`, so this is owed by the first caller that needs
      it. Raising the baseline is a real option and not a free one: the toolchain's multilibs are
      named for exact march strings, so a change is measured against them rather than assumed.

- [ ] **Give the converted fields their real ORDER.** M4.9.2 turned every cross-thread field
      into a relaxed `std::atomic`, which is a type change and nothing more: relaxed says
      nothing a second core will honour. What is left is deciding, per site, where an
      acquire or a release belongs. Three known residues: the 64-bit fields that had to stay
      `volatile` (a relaxed 64-bit load is a `__atomic_load_8` libcall), the six per-chip
      `_high`/`_last` clock anchors that `IrqLock` alone makes coherent, and
      the ring publication barrier, then still a consumer `-D` rather than a release
      store on the ring's now-atomic index. `docs/design-m7-smp.md` carries the reasoning.

### S3 inherits seven items, and each carries a constraint a grep does not show

Located 2026-08-31. The LOCATIONS are re-derivable and deliberately not written here; what is
written is what made each one more than it looks.

- [ ] **`g_isr_depth` and the fault-report depth are TWO distinct file-local scalars, not one.**
      Different names, different types, no reader in common, three lines apart in the armv8a
      backend. Both are safe today for the same stated reason, that the exception entry masks
      interrupts and secondaries park before running kernel code -- which is a claim about the
      PARKING and stops holding the moment peers run kernel code. Whoever fixes one must not
      assume the other went with it.
- [ ] **The per-core keying idiom already exists in TWO forms, so neither needs inventing.** The
      kernel layer keys an array by `arch_cpu_id()` inside an instance-local wrapper and asserts
      its own width; the arch layer arrays a block and seats it from a thread-pointer register.
      A fix picks the layer's own idiom rather than a third.
- [ ] **`Kernel::idle` and `Kernel::boot` sit immediately beside the keyed member that shows the
      idiom** and are plain scalars. The state inventory already classifies both per-core, so the
      document and the code AGREE on the target and only the code lags -- this is a gap, never a
      contradiction to resolve.
- [ ] **Padding the per-core block for cache lines cannot be done with `alignas` alone.** Its three
      field displacements are asserted against literals that assembly spells, and its SIZE is
      asserted against a literal the secondary entry spells too. So the block's size is an assembly
      constant and widening it edits assembly. And the tree deliberately makes no compile-time
      cache-line constant: the maintenance code reads the line size at RUNTIME from the cache type
      register, its comment saying why a part smaller elsewhere in the hierarchy would leave lines
      untouched at 64. Padding to 64 is therefore a compile-time bet that code next door refuses to
      make.
- [ ] **The 64 KiB kernel stack has two authorities with no shared symbol, and a THIRD nearby figure
      that must not be merged with them.** One is a backend constant, one a linker-script
      assignment; nothing relates them and no gate reads both. The trap is that a per-thread kernel
      stack size also exists and resolves to a different value on this arch -- a sweep for "the
      kernel stack size" finds all three and unifying them would merge two unrelated things.
- [ ] **The arrival timeout is a spin count, and the duration idiom it should follow is in the same
      file.** The timer arm converts nanoseconds through a tick figure derived from the counter
      frequency register, and that register is read earlier in the same function that later releases
      the secondaries -- so the frequency is provably live where the spin loop runs. Note a second
      spin-count bound in the same backend, so the arrival loop is not the only one of its kind and
      a fix should say whether it covers both.
- [ ] **The SMP predicate cannot meaningfully move to a chip file until a second chip exists.** The
      declaration is per arch and already names both controllers' routing registers in its own
      comment, which is the tell. But the family has exactly ONE chip, and what a chip declares
      about its controller today is a struct of bases, an interrupt count and a timer identifier --
      no CMake or Kconfig anywhere selects a GIC VERSION. So the per-part declaration is S6b's to
      make real; S3 owns only the semantics below.
- [ ] **Gap 2 is a TORN PAIR before it is a lifetime problem, and affinity is now real rather than
      assumed.** The interrupt entry takes no lock and reads a binding's handler and its argument as
      two separate unlocked loads, so the pair can be observed torn, and the event path then
      dereferences a raw binding pointer a concurrent teardown is freeing. Teardown's claim to
      safety is an interrupt-masked window, and the lock wrapper is a bare save-and-restore of the
      local core's mask with no lock word, so it excludes nothing elsewhere; masking a line does not
      retract an interrupt another interface already acknowledged. What changed in this step: device
      lines are pinned by a target bit each core PUBLISHES rather than by a hard-coded interface
      number, so N3's answer by affinity now names a core instead of betting on the numbering.
      Alongside it, the reference count beside those bindings is a non-atomic byte read-modify-write.
- [ ] **Gap 3 cannot be closed by lock scope, and the reason is that the switch-away IS the release.**
      The dying thread publishes its exited state inside its final critical section and the deferred
      switch fires as that section ends, so it is still executing on its own stack with its own
      context as the pending save slot while the allocator may claim that slot, bump its generation,
      push the still-live stack onto a free list and release its capability chunks. Publishing
      earlier is refused where the code says so; publishing later is impossible, the write having to
      precede the reschedule that both wakes joiners and hands the CPU away. So widening a scope only
      moves the release point: this wants an epoch or an off-CPU quiescence flag.
- [ ] **The doorbell's interrupt group is the reset value and the secure posture is unexercised.**
      The group register is left as reset with the non-secure attribute clear, matching the
      acknowledge and end-of-interrupt path already in that file. The security-extensions posture
      cannot be exercised on this bench at all: that machine option enters at EL3 and the port's own
      exception-level refusal fires first, from every core at once.
- [ ] **Keying the idle thread makes one guard PERMISSIVE rather than merely wrong, and that is the
      shape to look for wherever a per-core accessor gained an index.** The slay path refuses a
      target that IS the idle thread; with one cell per core the accessor answers for the CALLING
      core, so slaying a PEER core's idle thread passes the guard and ends that core's scheduler
      fallback. Asking "is this thread an idle thread" is a question over the SET of cells, which
      the accessor's signature cannot express -- so the fix is a predicate, not an index. The fault
      path's own five comparisons are safe by contrast, each comparing against the current thread,
      so both sides are the calling core's. Nothing is reachable while secondaries park masked.
- [ ] **Registering the idle thread fills only the creating core's cell.** The scheduler's add path
      runs on whichever core creates the thread, so it cannot fill a peer's; nothing in the tree
      creates more than one idle thread. Per-core idle creation is what closes it, and until then the
      peer cells stay null, which is unreachable for the same parking reason.
- [ ] **The mask triad's banked-register question has fourteen production call sites and none names a
      core.** Every one passes a line only, and the seam's own contract says the three are
      self-bracketed so a caller need not hold the interrupt lock. That contract is what makes the
      absent core parameter correct at one core; S3 decides what it means when threads run on more
      than one, and whatever it decides reaches all fourteen.

## Gate-surface re-inventory (anytime coherence, not scheduled)

Parked by the user as "at M7" when M7 meant the seam rework; M7 is multicore now, and this is
orthogonal to every milestone rather than gated by one. The user's framing, 2026-08-21: the gate
surface grows exponentially and may not be worth its size.

- [ ] **A MUTATION HARNESS MUST `touch` EVERY SOURCE IT RESTORES, OR THE CONFIRMING BUILD IS
      STALE AND THE GREEN IS THE MUTANT.** The rule for anyone mutation-proving an arm here, and
      it applies to `ninja` and to `make` alike. A harness that copies a source aside, patches it,
      builds, runs and then moves the copy back hands the build a restored file whose mtime is the
      COPY's, which predates the object just built from the mutated source. The build believes the
      object is current, the "reverted, green again" run re-executes the MUTANT binary, and the
      red reads as a regression in the arm just written rather than as a stale object. Only the
      restore direction is poisoned, the mutated builds being newer than their objects, so the
      symptom is always a red that appears AFTER a mutation batch and never during one.
      `touch` every restored source before the confirming build, or copy with the mtime bumped.

- [ ] **Re-inventory the test-gate surface.** M4.5.9 root-caused the binary-introspection gates'
      silent-failure paths without rewriting them; whatever is still oversized then is this pass.
      Take the inventory against the surface as it is, do not pre-design it here. Baseline at
      M4.5.8: 23 shell scripts, ~2,000 lines, plus 5 Python checkers -- STALE, predates the `tests/`
      reorganisation: the shell scripts now live under `tests/static/` and `tests/integration/`,
      unit gates under `tests/unit/`, and the host unit layer is GoogleTest with per-case `ctest`
      entries rather than scripts. Re-derive the count when this pass runs; do not carry the old
      numbers forward as current.

- [ ] **Give every image gate a `QEMU_TIMEOUT` bound of its own, then delete the nine job-level
      pins in `ci.yml`.** That is the rule the `bench` job already states: a bound belongs in the
      gate, and a job-level value overrides every script's `:=` at once. The pins are still there
      because 28 of the 54 `tests/integration/check_*.sh` set no bound, and for those the pin is
      the only backstop above `tests/lib/gate.sh`'s 20 s in `run_image` and 8 s in `poll_image`.
      All nine are RAISED to 180 for that reason rather than dropped.
      - **IT IS NOT A MECHANICAL SWEEP, because the scripts are shared across jobs.**
        `check_qemu_panicgate.sh` runs in `qemu-arm`, `qemu-riscv64` and `qemu-x86_64`;
        `check_qemu_hello.sh`, `check_rootgone.sh` and `check_fault_dump.sh` spread the same way.
        All nine pins read 180 now, so every unbounded gate in all of them sits on one figure
        nobody sized for it, and a default written into one of those scripts has to be the worst
        of every job it runs in or it cuts that gate somewhere. Each bound has to come from what
        that gate costs, measured, and not from the pin it happens to inherit today.
      - **DISCHARGED: no pin cuts a gate it runs any more.** `check_qemu_selftest.sh` sets 180
        and is registered in all nine pinned jobs, so `qemu-arm64`, `qemu-arm64-gicv3`,
        `qemu-riscv`, `qemu-riscv-mpu` and `qemu-x86_64` were clamping it to 30 and
        `qemu-arm64-amp` to 90. All six now read 180 and carry the same comment the other three
        do. Each value was chosen from that job's own enumerated test list, and 180 is the
        largest own-bound in all nine. The one figure above it that a job touches is
        `check_bench_irqspan.sh`'s 400, and `qemu-riscv` selects only that gate's `--controls`
        entry, which boots no image, so the 400 is not a bound that job has to clear. The
        deletion this parent item asks for is still owed and still needs the per-gate bounds
        first.

## RISC-V switch cost (M8.11) and two `Later` hardening items -- retagged after the M8 cut

These three carried a shared `M8` tag from the era when "M8" meant "the last milestone".
`roadmap.md`'s M8 section rules that only the RISC-V item belongs in the milestone as cut, on its
merits, as M8.11 / P9 (see the M8.11 section above); the other two share nothing with M8 but the
word MPU and stay in `roadmap.md`'s `Later`. Retagged below; the technical content of all three is
unchanged.

- [ ] **ARMv8-M TrustZone kernel-confinement backend -- opt-in, per-chip** (Later, fable-gated,
      needs the M4 service model + M6 SMP settled). The armv8-M-with-Security-Extension mechanism for
      kernel confinement: kernel/TCB in Secure state, apps in Non-secure. NOT per-task isolation and
      NOT an MPU replacement (MPU_NS still does all per-task work, same per-switch cost); it is the
      strongest armv8-M realization of "Option B" (confine the kernel), layered ON TOP of Option B,
      not instead of it. Buys a hardware TCB boundary (NS-privileged cannot touch Secure memory) + a
      PSA-style secure-services partition for roots-of-trust that fits the capability-gated-services
      model. Machinery: SAU/IDAU partition, secure-gateway veneers + S/NS call ABI, banked SPs, NVIC
      ITNS interrupt targeting, a separate Secure build/link. Per-chip capability -- M23/M33/M55/M85
      MAY implement it, detect + fall back to Option B alone; RP2350's M33 is a concrete target (also
      the PMSAv8 + SMP target). Security/assurance play, not perf. The M6 dependency is mechanical.
      The MPUs and the SAU are both banked per core, so a TrustZone SMP story must set up the
      S/NS partition on each core separately.
- [ ] **Confine the trusted kernel with an explicit MPU map ("Option B") -- FLEET-WIDE hardening**
      (Later, fable-gated, per-arch). Today privileged/kernel execution runs UNCONFINED on each
      backend's permissive background; a kernel wild pointer rides it silently instead of faulting.
      Option B removes that background so even the kernel is confined and a stray kernel access
      FAULTS (defense-in-depth / debuggability -- catch our own bugs early; NOT a security boundary,
      the kernel is trusted). This is NOT a bug fix anywhere -- the M7 speculation stall is already
      closed by "Option A" (wrap the leaky external Normal bands, keep PRIVDEFENA;
      `docs/design-teensy-mpu-hang.md`); no other arch has that stall. Per-arch mechanism:
        - armv7m/armv6m PMSA (XMC/F411/RP2040/microbit): drop PRIVDEFENA + region-0 4 GiB
          Strongly-ordered/no-access/XN floor + explicit kernel regions (code RX, RAM RW, periph
          Device). M0+ is region-tight (8 descriptors).
        - K64F SYSMPU: restrict RGD0 (today supervisor-full) + explicit supervisor RGDs.
        - RISC-V PMP (C6): LOCKED PMP entries (bind M-mode too).
        - RX-MPU (RX72M): restrict the supervisor region set. Xtensa (WROOM): N/A (no MPU).
      Cost: forks the fleet-wide "privileged = background" contract every board rests on (incl. the
      armv7m non-pow2-arena-drop path) -- needs a per-arch fable pass + probe-ful bring-up.

## thread_join is NOT a flake either: 80% on frdmk64f + uartirq, and nowhere else (2026-08-10)

Found by applying the `rr_interleave` lesson one board over -- measure a RATE before writing "flake".

| board + service list | runs | `thread_join` failures |
| --- | --- | --- |
| `frdmk64f` + `kickos_services_frdmk64f_uartirq` | 5 | **4** (7 runs total across the day: 5) |
| `xmc4800-relax` + `..._xmc4800relax_uartirq` | 5 | 0 |
| `rx72m` + `..._rx72m_uartirq` | 5 | 0 |
| `esp32-wroom` + `..._esp32_uartirq` | 1 | 0 |
| `esp32c6-wroom` + `..._esp32c6_uartirq` | 1 | 0 |

**80% is not marginality, it is a defect**, and it is K64F-specific and IRQ-service-specific: the same
board on its DEFAULT list (the polled UART0 console + `k64dspi`) is 95 ok clean, and every other board is
clean under its own IRQ list.

**It is NOT the PendSV pair race.** That fix is in this tree and cured `rr_interleave`; this survives
it, so it is a different cause.

The assertion is `waited_us >= JOIN_PARK_US` at `main.cc:5395` against `JOIN_PARK_NS = 20000000`
(20 ms): the target sleeps 20 ms and the join must not return before that. So **the join returns
EARLY** -- either a spurious wake or a mis-measured elapsed time.

**The first thing to check, and why:** `CONTEXT.local.md` records that the K64F's **DWT is dead**, so
its cycle counter is unavailable and only wall-clock is valid. If `waited_us` is derived from
anything DWT-shaped on this board it would under-report and the arm would fail while the join
behaved correctly -- an instrument fault, not a kernel one. That distinction decides whether this is
a real early wake (kernel) or a bad measurement (test). Rule the instrument out before chasing the
scheduler, because a 20 ms park is long enough that a real early wake would be a serious IPC defect.

Note the discipline that surfaced this: it had already been A/B-proved identical before and after the
service rework, which made it easy to file as pre-existing and stop. Pre-existing is not the same as
harmless, and a rate is what tells them apart.

## exit() scope is CHIP-defined, not app-defined (ruled 2026-08-07)

The ruling: on an MCU a thread IS the unit of isolation, so a thread and a process are the same
thing and `exit()` in a thread ends that thread. On an A-class part with an MMU and real processes,
`exit()` from any thread ends the WHOLE PROCESS, which is what POSIX says. So the scope follows the
target's process model. It is a platform fact derived from the memory model, never a knob an
application author sets: an app that could choose would be choosing whether its peers die.

**Consequence for the tree today, and it inverts the open question in `m4.8.1: the services exit
through the C library`.** Every current target is the MCU case, so `exit()` must mean thread-exit
everywhere, and the cross ports do NOT deliver that. Disassembly of `exit` in the RX image: it calls
`__call_exitprocs` (the `atexit` / `__cxa_atexit` list), then loads `__stdio_exit_handler` and calls
it if non-null, and only then reaches `_exit` -> `kos_exit`. Both are IMAGE-global teardown, and
running image-global teardown because one thread ended is wrong even under thread-equals-process:
newlib assumes one process per image and KickOS puts many threads in one. Provably inert right now
(nothing registers an atexit handler and KickOS uses its own `kprintf`, not newlib stdio), and the
`.ld` `ASSERT(.fini_array empty)` does not cover it, because that assert governs STATIC registration
only. It goes live the first image that links real stdio, which the consumer-API principle
("standard `printf` / `std::cout`") invites.

`user/src/sim_exit.cc` already does the right thing and is the model: override `exit()` outright and
route it to `kos_exit`. Do the same on the cross ports. Overriding `exit` and not `_exit` is not a
style choice: glibc's `exit()` calls its own hidden alias, so an `_exit` definition is never reached
(measured when `sim_exit.cc` was written).

When the MMU era arrives (`docs/design-mmu-era-exploration.md`), the same seam flips rather than
grows a second mechanism: on a target with processes, `exit()` ends the process and the thread-scope
primitive stays `kos_exit`.

## M4.8.x TRIAGE: the 30 items the three milestones left, sorted by what they actually are

**Why this section exists.** A review finding has exactly TWO honest dispositions: **FIX IT, or ASK
FOR A DECISION.** Filing is not a third one -- it is what you do with class D below, and nothing else.
Across this file the practice was the opposite: 200 open against 207 closed, every review filing 10-18
while closing 1-3, and the pile is undifferentiated so an accepted trade and a latent defect read
identically at a glance.

**And do not reach for "no mechanism blocked the merge" -- there is no such mechanism to have.** A
gate is not an artifact: a ctest entry, a static check, even the arm that proves a fix can all be
DELETED, which is precisely why the gates in this tree are mutation-tested rather than trusted. The
discipline is the gate. Recording a defect in this file and merging anyway is a choice, never a
process outcome.

**So the rule for a future review: fix each finding, or raise it as a decision, before the merge.**
Only class D is filable.

**A -- LATENT DEFECT in shipped code (6). ALL SIX ARE NOW ADDRESSED, and TWO WERE MIS-FILED in ways
that changed the answer.** Kept in full because the mis-filings are the lesson:

**B -- ACCEPTED WITH A MEASURED COST (3), and these are the ones that should have been raised as
decisions rather than filed. RAISED, and the ruling is that ALL THREE GET FIXED.** The ABI grows a
second verb: kill stays cooperative (0 = ACCEPTED, death at the next syscall ENTRY) and **SLAY** is
the forcible half, SIGTERM/SIGKILL. **The mechanism is NOT a reaper** -- no stranger ever runs another
thread's `cap_teardown`; the victim runs its own, because the seam rebuilds `next->ctx` inside
`switch_to` before `arch_switch`. Two premises died on the way: `arch_fault_redirect_to_exit` cannot
be reused (not relocatable to a saved context on any backend, and on armv7m and rxv3 it reads AND
CLEARS sticky fault status, so calling it off a fault destroys the reporter's evidence -- the seam is
`arch_context_init`), and the termination argument is NOT the RR slice timer, because the clock is
TICKLESS and an all-FIFO image with no sleeper has no periodic interrupt at all. The real argument:
on one core a target that is not the caller is never RUNNING, so READY and BLOCKED are total.
Four non-reorderable steps, zero `.bss` on each. Design record still owed to `docs/`.
- [ ] **SILICON OWED FOR S3/S4 ON TWO OF THE THREE.** `rx72m` IS PAID: the `m484capirq` capture
      recorded below is a tree carrying S3/S4, and the five arms are registered unconditionally, so
      its `1..104` covers them. Still owed: `esp32-wroom` (`lx6` -- the immediate-switch backend
      with two resume formats, and the ONE backend whose seam claim is a reading of the code rather
      than a run) and `picopi` (the only armv6m enforcement unit). The
      five arms to look for in a selftest stream are `thread_slay_window`, `thread_slay_gate`,
      `thread_slay_timeout`, `task_slay_group`, `task_slay_gate`.
- [ ] **Two fail-closed guards in slay are UNREACHABLE from userspace and their mutants SURVIVE**
      (`docs/design-kill-and-slay.md` section 14.5): the idle/privileged target refusal, because
      idle is a static TCB outside the `ThreadPool` and root is unprivileged, so no privileged
      thread is resolvable by handle at all; and `kos_task_slay`'s caller-is-a-member refusal,
      because a member cannot be its own group's creator. Both become reachable in the driver era
      or under M5. `kickos_fault_kill_thread` carries the identical rule with the identical
      reachability status, so this is a pre-existing shape rather than a new one.
- [ ] **The `sched::wake` residue, moved here from class A because it is an ACCEPTED COST and needs a
      ruling.** A superseded publication keeps a `switch_count` it never earned and an RR slice armed
      before it ran. **It is UNDETECTABLE IN C**: no state separates "published, switch pended, not
      fired" from "fired and running", and any marker written at the pend is stale from the fire
      onward, so it cannot recognise the supersede it would have to undo -- on the sim/lx6 inline-swap
      path a stale marker is WORSE than the current code. Removing it needs the arch to report that a
      pended switch completed, or `need_resched` consumed at the mask boundary: five backends plus the
      K-seam stub. Cost of leaving it: bounded by one quantum, self-limiting, fairness only.

**C -- GATE AND TOOLING DEFECTS. FILED AS 4, CLOSED AS 10** -- the filings under-counted, and every
one of the six extras was a gate that had silently stopped gating or had disclosed a hole it never
closed. The first seven are mutation-proven; the two S3/S4 found are below them and each has its
own proof shape.

**A REGRESSION M4.8.4 INTRODUCED, BISECTED ON SILICON, FIXED, AND RE-MEASURED THERE. CLOSED.**
`rr_interleave` fails on `rx72m` under `kickos_services_rx72m_uartirq`. Measured rates, one board,
same app, TAGs `m484rx*`:

| tree | list | result |
| --- | --- | --- |
| `c87f84ed` (master, M4.8.3) | `_uartirq` | 0 of 3 fail -- `rr order: ABABAB` |
| its parent | `_uartirq` | 0 of 3 fail |
| **the creator-hold commit** | `_uartirq` | **3 of 3 fail -- `rr order: AABBAB`** |
| S2 | `_uartirq` | 3 of 3 fail |
| the branch tip | `_uartirq` | 3 of 4 fail |
| the branch tip | DEFAULT | 0 of 3 fail |

The parent is green and the creator-hold commit is red, so the regression is that commit. ISOLATED to
one line by building it with the other line kept: removing `task_orphan_created_by` and keeping
`c->task = nullptr` is green 3 of 3. So the cause is the creator-hold sweep, which is
`KICKOS_MAX_TASKS` (= `KICKOS_MAX_THREADS` + 1, so 17 on rx72m) byte compares MASKED, on EVERY
thread exit.

**THE CAP_IRQ PRE-PASS, the last code finding of the ten-angle review, FIXED AND WITNESSED.**
`cap_teardown`'s IRQ pre-pass scanned the whole capability table interrupt-masked on EVERY thread
exit -- the same shape as the `task_orphan_created_by` regression this milestone shipped and fixed,
and its own comment conceded it ("the scan by the table's own width"). It could not simply be
chunked: a gap inside that pass is a moment when a thread with a counted teardown depth still holds
an IRQ line, and both console-reclaim sites rely on that being impossible.

**D -- COVERAGE, TEST-INFRA, DOCS, PERF, FUTURE (17).** Ordinary backlog. Includes the whole M6/SMP
group and every "no arm for this yet" item. Added while closing A, B and C -- all genuinely class D,
which is the only reason they are filed rather than fixed:
- [ ] **The f302 capture-protocol fix is untracked.** `.session/bench-capture.sh` is gitignored, so
      the one repo-visible trace is a comment in `tools/flash-stlink.sh`. Nothing gates it and a fresh
      checkout does not have it. Either the bench scripts become tracked tooling or the knowledge
      stays a comment plus `CONTEXT.local.md`.
- [ ] **The `kickos_terminate` device drain has NO witness.** Contract and plumbing only, argued from
      the seam. `arch_console_flush_sync` has a body on `mk64f` and `xmc4800` only, plus `stm32f302`
      now, so on every other board the drain is still a no-op and the terminal path can still cut its
      last line. Per-chip bodies are fleet work.
- [ ] **`kernel/bench/bench.cc`'s `s_*` fix is gated but unwitnessed under `KICKOS_BENCH`** -- the
      gate is a source scan, and no bench image was run.
- [ ] **`task_cancel_group` and the endpoint drain publish twice under one `IrqLock` from a LIVE
      thread**, the same residue as the `sched::wake` supersede but with no dying guard involved, and
      more reachable. Not filed anywhere before.
- [ ] **`teardown_depth` now has one production consumer and one fixture consumer**, so the
      `cap_teardown_active()` gate in `exit_current` is premise-less. Kept as conservative retry
      throttling; revisit when the DEV-window respawn gate lands.
- [ ] **`console_crlf` now also gates console write POLICY** and should be renamed; deferred
      deliberately to avoid fleet record churn mid-milestone.
- [ ] **No reply-bearing flush op**, so the init cannot read the residual `flush()` returns.
- [ ] **CDC console throughput is ~167 B/s** under many small writes (2008 bytes in 12 s). A console
      you cannot read a suite through is marginal.
- [ ] **`docs/design-m4.8.2-host-unit-tests.md:435` says the K-seam is sixteen symbols; it is
      eighteen**, ten of them `arch_*`.

## Found landing the M4.8.2 host unit-test layer (2026-08-11)

The layer's record is `docs/design-m4.8.2-host-unit-tests.md`; section 8 is what landing it found.
Items 5 to 7 of its section 7 are still owed and are the ones below plus the migration.

- [ ] **The K-seam fixture's OTHER self-diagnostics have no death case, and now there is a macro for
      them.** `KICKOS_EXPECT_FIXTURE_REFUSAL` (`tests/unit/kfixture/kseam_test.h`) gates the
      no-waker refusal; the same shape would gate `reset()`'s in-flight-sweep refusal, `note_park`'s
      "parked with no arm waiting for it", `kickos_terminate` and the range checks in `spawn` /
      `seat_pool` / `task`. Each is an `exit(1)` a mutation cannot currently be caught by, and the
      `reset()` one needs an arm that deliberately abandons a sweep, which is the entry below.
- [ ] **`tests/unit/kfixture/reset()` cannot clear `g_cap.teardown_depth`**, because `cap.cc` keeps its
      `CapState` in a TU-local `constinit` that the `kernel() = Kernel{}` assignment does not reach.
      It refuses loudly instead (`cap_teardown_active()` at the top of `reset`), so an arm that
      abandons a sweep stops the suite rather than poisoning every later arm. Widening
      `cap_slab_init()` to zero the depth was considered and REFUSED: no arm needs it, and widening
      shipped code for a fixture's convenience is the wrong direction. Revisit only if an arm
      legitimately needs to abandon a sweep.

## Found by the M4.8.2 ten-angle review (2026-08-11)

Five reviewers over ten angles, against `b77a3ef4`+`a2695e08`. No Critical: no isolation or
memory-safety escape was constructible from the new mid-sweep preemption, and every protection
`design-m4.8.2-host-unit-tests.md` section 8.2 relies on was verified independent of the guard. What
follows is what survived that.

- [ ] **`sched::wake`'s guard reads a `kernel().current` that a deferred switch has already moved.**
      `switch_to` publishes `kernel().current = next` BEFORE `arch_switch`, and on ARM, RISC-V and RX
      that only pends. So after one admitted wake inside a sweep the dying thread keeps running with
      `c` naming the PEER, and every later wake in that chunk sees `dying == false` and
      `state == RUNNING`: both new clauses are dead and `reschedule()` runs unconditionally. No wrong
      final state was constructible, because the EPIPE drain pops in DESCENDING priority so
      `pick_next` returns the already-published peer and the extra `reschedule()` early-returns. The
      residue: the guard's stated premise is defeated for later wakes, and two arms in ONE chunk
      waking ASCENDING priorities would run `on_switch_in`/`arm_slice` for a peer that never runs, so
      an RR peer forfeits part of its first quantum. The gate cannot see it (the stub also returns).
- [ ] **`f302nucleo`'s skip and partial sets are declared nowhere in the tree.** `microbit` is the
      only board whose expectations are stated (`user/apps/common/selftest/CMakeLists.txt`), so a
      hand-run of `check_tap_stream.sh` on f302nucleo has to be handed sets taken from the log being
      judged, which is self-confirming. Declare them the way microbit's are.
- [ ] **The K-seam fixture only ever compiles the SIM posture.** It takes `kickos_kernel`'s
      `COMPILE_DEFINITIONS` verbatim and registers only under `KICKOS_ARCH STREQUAL "sim"`, so a
      SEGMENTED capability table (`KCAP_RUN_CHUNKS > 1`, which `frdmk64f` runs), `KICKOS_DIAG_TERSE`,
      and every non-sim cap geometry never reach a K-seam arm. That compounds with the chunk-boundary
      arm: flat-versus-segmented teardown IS a chunk-boundary property.
- [ ] **Splitting `user/src/syscall_stubs.cc` per subsystem breaks the U-seam shadow protection
      silently, and this tree has a PRECEDENT for doing exactly that.** `user/CMakeLists.txt` already
      splits `sim_exit.cc` and `newlib_sbrk.cc` into TUs of their own precisely to control extraction
      granularity, so the refactor is the file's own idiom rather than a hypothetical. Secondary
      condition nobody has stated: the collision is loud only while the image references at least one
      symbol from that member the shadow does not define, which is true of `selftest` and not
      established for a minimal image.
- [ ] **Swapping `Thread::privileged` and `Thread::dying` would save a load on EVERY wake, free.**
      Measured on armv7em: `state` is at offset 67 and `dying` at 69 with `privileged` between them,
      so no single `ldrh` covers both and the guard pays two byte loads. Both are `bool` and both sit
      in the same padding band the header already documents as free, so the swap costs no footprint
      and the `sizeof(Thread)` asserts would catch a mistake.
- [ ] **A K-seam gate cannot register a fixture refusal, and cannot print `not ok` when passing.**
      `kickos_add_kseam_gate` now sets `FAIL_REGULAR_EXPRESSION "not ok"` unconditionally, which is
      what stops a forged exit code, and the cost is that the fixture's own refusals cannot be gated
      through that function and no future gate may quote a TAP-shaped expectation in a diff printer.
      An `EXPECT` parameter that replaces the fail regex would cover both.

## Found porting fault isolation to rxv3 (2026-08-12)

- [ ] **`kickos_fault_below_stack` NARROWS WHAT A FAULT REPORT CAN ATTRIBUTE ON rxv3, and design 4.3's
      reaper is what removes it rather than tuning it.** RXv3 cancels the faulting instruction and
      restores SP (ISA UM sec.5.3.1), so no SP-based test can see a stack overflow and the faulting
      ADDRESS is the only evidence. The test is EXACT for the overflow class -- a stack grows down, so
      the first denied access is beneath the base by construction -- but its converse is not, and the
      cost is MEASURED on one tree: `mpu_fault`'s cross-domain write to `0x13200`, below `domainA`'s
      stack, escalates to the panic dump instead of dying alone, while `rxdrv`'s `0x8c068` above the
      stack dies alone. So on this board an unprivileged operand access to any LOWER address is
      reported as a system panic and the thread is not credited with dying alone. **Do NOT replace the
      stack base with a distance threshold**: a frame larger than the threshold puts privileged code
      back on an exhausted stack, which is the UNSAFE direction and is the exact defect this closed
      (`.session/logs/m483rxovf-*`, which reached `PC=0x0`). The real fix is a stub that never runs on
      the dying thread's stack, i.e. `docs/design-m4.7.9-fault-isolation.md` section 4.3, which
      already recorded that it "would survive 4.2" and is now measured on an ISA that needs it. It
      would delete the rxv3 test, not tune it.

- [ ] **On a FLAT rxv3 board nothing catches a wild SP that faults with a non-MPU cause.** The
      below-stack test is inside `#if KICKOS_HAVE_MPU` because it reads `MPESTS`/`MPDEA`, and the USP
      containment test only sees an SP that is out of range, not one that is in range with no room
      below. So on `rx72m-flat` an address exception taken by a thread whose stack is exhausted would
      be killed and the stub would run on it. Not witnessed either way -- the flat posture has no arm
      that reaches this -- and it is the same hole 4.3 closes. Filed so the enforcing-only scope of the
      guard is on the record rather than implied by an `#if`.

## Found landing task-layer step 9.3 (2026-08-11)

- [ ] **`microbit` has ZERO arena slack, so no `.bss` addition is ever inert there again.**
      `__kickos_ram_start` IS `_ebss` and the granule is 32 bytes, so four bytes cost as much as
      thirty-two. Step 9.3's task pool took `mem_self_grant`'s last `kos_ram_alloc` grain and that arm
      is now a declared skip. The structural options, none taken: shrink the board's static demand,
      give the selftest a microbit-specific arena reservation the way `uart_service` got one, or
      accept that this board's skip set grows once per milestone. Worth deciding deliberately rather
      than one arm at a time, because the next `.bss` byte from ANY change takes the next arm.
- [ ] **M6/SMP: the claim-then-commit shape in `domain_for` and `task_for` is safe only because
      `IrqLock` is enough on one core.** Both hand out a pool slot at refcount 0 and are committed by
      a later `domain_ref`/`task_ref`, and what makes the window atomic is that `thread_create_call`
      declares a FUNCTION-SCOPE `IrqLock` as its first statement, spanning the claim, the thread-slot
      alloc and `thread_create`. `IrqLock` masks LOCAL interrupts only, so under SMP a peer core can
      claim the same slot: two threads would then share one task and the loser's domain would sit at
      refcount 0 as a free slot while a live thread names it. Verified NOT reachable today (no second
      `IrqLock`, no `arch_irq_restore` and no `reschedule()` between the claim and the commit; the
      nested lock in `assign_thread_id` restores "masked" rather than "enabled"). Pre-existing in
      `domain_for` and inherited unchanged by the task layer, so it belongs to the SMP work and not to
      M4.8.3. The neighbouring comment about snapshotting `p->caps[ci]` is a DIFFERENT seam, about
      user-memory TOCTOU, and is not evidence either way.

## Found landing task-layer steps 9.4 and 9.5 (2026-08-12)

- [ ] **A cancelled thread that never re-enters the kernel is still unreachable.** The death point is
      the syscall entry, so a pure compute loop survives a `kos_thread_kill` and a `kos_task_kill`
      indefinitely. Preemptive cancellation is the only fix and it is a much larger change: it needs a
      point at which one thread may run a stranger's `cap_teardown`. No in-tree thread shape has this
      problem (all of them loop through a syscall), so this is a documented floor rather than a live
      gap -- but it is the reason 0 from either call means ACCEPTED and not GONE.
- [ ] **"Release the DEV window BEFORE the capability sweep" has NO gate, and never had one.**
      M22 and M25 in the 9.4/9.5 mutation run (`docs/design-task-layer.md` 8.3) are this entry,
      filed rather than killed. Measured by mutation at the 9.5 tree, both ways. `dev_window_free`
      skips a `dying` thread so a supervisor woken by the sweep's EPIPE can respawn into the
      window at once; deleting that arm
      leaves every suite green on `sim`, `qemu` and `qemu-riscv`. So does the ORDERING it preserves:
      moving `task_release` from before `cap_teardown` to after it is equally invisible. The second
      result is what says this is INHERITED and not something 9.4 introduced -- before it, the same
      exclusion came from the domain reference being dropped at the top of `exit_current`, and that
      was untested too. What a gate needs is the full choreography: thread A holds window W, A exits,
      and DURING A's teardown the EPIPE wake reaches a supervisor that respawns into W and must
      succeed. `sim_driver_death` case 3 has every piece except the respawn. Until then the comments
      at both sites are the only thing holding the invariant, which is why they say so.
- [ ] **Nothing witnesses the DEATH POINT on silicon.** `tests/unit/taskdeath` gates that a peer is
      marked and made runnable, `sim_driver_death` and the `task_group_kill` selftest arm gate that it
      then dies -- all under a host or emulated clock. The interesting case is a driver cancelled while
      its IRQ line is armed on real hardware, where the wake races the device. Wants an enforcing board
      with an IRQ UART service list, which is what makes it a fleet-pass item and not a gate.

## Found during M6.3 and M6.4, deferred with the reason, then fixed at the pre-audit step (2026-08-28)

- [ ] **OWED MOVE: the three byte-seam DEFINITIONS are in the syscall layer while their declarations
      are in the memory layer.** `kaccess_from_user`, `kaccess_to_user` and `ep_copy` are declared in
      `kernel/include/kickos/aspace.h` (lines 46-52), which is where R6 moved them from
      `kernel/syscall/syscall_internal.h`, but they are still defined in
      `kernel/syscall/syscall_mem.cc` (lines 387-408) over a file-local `access_copy` (line 307).
      They arguably belong in `kernel/mem/aspace.cc` beside the rest of the address-space code, and
      the move did not happen only because another agent owned that file during the pass. No
      behaviour depends on it; it is a layering tidy-up with one caller-visible consequence, that a
      reader following the header lands in the syscall directory.

- [ ] **RESIDUAL: `KICKOS_RV64_SSTATUS_SUM` is still defined and is now used by nothing.**
      `arch/riscv/rv64imac/include/kickos/arch/rv64_frame.h` line 95 defines it; R6 removed both
      writes and the assembler `.equ`, and a tree-wide grep finds no other reference. A dead constant
      naming a bit the port has decided never to set is the kind of thing a later edit picks up and
      uses, so it should either go or carry a one-line comment saying the port never sets it and why.
      Left alone here because the file was not this pass's to edit.

## No gate covers a shell tool's use of a non-POSIX utility operand (2026-09-06)

- [ ] **A PORTABILITY DEFECT IN A SHELL TOOL IS INVISIBLE TO EVERY GATE IN THIS TREE, and the
      one that looks like its owner cannot see it.** `tests/static/check_awk_portable.sh` is a
      NAMED-SPELLING check: it holds a list of gawk-only function names and greps the awk
      programs under `tools/` and `tests/` for a call to one. A utility OPERAND is not a name on
      that list and never can be, so `dd status=none` sat in `tools/amp/merge-partition.sh`
      through every green run this tree has ever had. The rest of the static set is no closer:
      `check_shell_special_names.sh` reads shell-owned variable names, `check_dash_punct.sh`
      punctuation, `check_ascii.sh` bytes.

      **THE FAILURE MODE IS THE EXPENSIVE ONE: works here, refuses there.** A dd, sed, awk, grep
      or find operand the GNU tool takes and a busybox or BSD one does not is invisible on this
      box and on a CI image built from the same distro, and a dd that does not know an operand
      refuses the WHOLE invocation rather than ignoring it. So the tool does not degrade on a
      minimal image, it stops, and it stops in whatever the tool was doing at the time.

      **`tools/esp-x86_64.sh` IS A DECLARED EXCEPTION AND NOT AN INSTANCE.** It requires GNU dd
      (`bs=1M conv=sparse status=none`), its header says so, and it probes the operands and
      refuses before it writes anything. A gate built later has to tolerate a script that states
      its requirement, or it will report the one file in the tree that already did the right
      thing.

      **THIS IS RECORDED, NOT SCHEDULED.** A whole-tree lint is neither a unit test nor an
      integration test, which is what this tree runs, so adding one is the owner's call and not
      a reviewer's. What it would take: a list of operands per utility rather than per name, the
      same both-directions self-test the awk gate carries, and an answer for the scripts that
      legitimately require GNU tools.
