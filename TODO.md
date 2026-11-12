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

**WHAT M10 DELETES, SO THAT ONE WAY IS LEFT.** The per-board service-list arrays and the
service kinds they carry. The per-board pin-map tables. The service-list selection knob and
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
      against the result. The esp32-wroom-smp silicon selftest and the pizero2350-amp2
      partition's ACCESSCTRL gate capture were witnessed on 2026-11-08 by M10.6.11's bench pass
      (`git log -S'THE BENCH PASS, BOARD BY BOARD' -- TODO.md`).

- [ ] **M10.6.12, FIRST: THE TEST SUITE AUDIT, NO TEST CODE IN THE KERNEL, AND THE BENCH OUT OF IT
      (maintainer, 2026-11-09: done before the rest of M10.6.12, to gain time on every chain after it).** The closing chain takes about 11 hours, 8 of them the stress run,
      and the maintainer finds the suite bloated. Measure first: each test's time across a chain,
      and which tests have ever failed. Then:
      - **Rerun only what changed: PARKED (maintainer, 2026-11-10) until a more stable version.**
        An image test's verdict kept for an image whose bytes, stamps aside, and whose judge are
        unchanged; the same for a preset's host tests and the inputs that preset builds from.
      - **Size the stress run to what it finds.** Selftests that never failed only under repetition
        run fewer times; the full stress run is kept for kernel changes, as its own stage.
      - **Audit the integration tests, image tests and unit suites** with the four questions the
        M10.6.2 gate audit asked: which bug class, has it ever fired on a real change, can the
        compiler or the linker hold the rule, is it at its level. Delete the duplicates and the
        dead ones.
      - **No test code in the kernel or arch code**, as `docs/reference/process.md` rules
        (*Tests*, "The code under test is the production code"). Each
        fault-injection build option goes, after the clock fallback, the CCR plant and the repass
        witness, which went to host tests. The node witness option goes. `KICKOS_WITNESS_UACCESS`, `KICKOS_WITNESS_REWIND`, `KICKOS_SPLIM_PLANT` and
        `KICKOS_SPLIM_PLANT_UNLIMITED` are kept as named exceptions (maintainer, 2026-11-10): the
        only fault-injection options kept. The clock
        fallback is the first case: it stays on the reset clock and sets the console baud for
        it, both computations a host test holds, so its bench items in "M10.6.12: THE BENCH ITEMS
        M10.6.11 LEFT OPEN" go with the option.
      - **The bench out of the kernel** (maintainer: "we created a cancer inside the kernel code
        ... at one point we will need to wipe it"). The kernel keeps nothing that exists only for
        the bench: the kernel's bench directory, the bench system call operations and the bench counters on the
        switch and syscall paths go, and the bench measures from user space with the
        user-readable clock and from the trace. Until then nothing is added to it, and the walk
        slot count, a bench app setting kept in kernel Kconfig, goes with it.
      - **Held for later, from the test audit (2026-11-10):**
        - Replace the fixed-time waits with a wait on the event in `capture_teardown`,
          `dash_punct`, `reboot_reset` and `wallclock`; `capture_teardown`'s 0.5 s wait failed
          under load.
        - Move `panicgate`'s text-format cases 2, 4 and 5 and four selftest refusal arms to unit
          tests (unsure; to check).
        - Replace `initwalk`'s scripted fake kernel (about 3k lines) with the real kernel through a
          bridge.

- [ ] **M10.7: `kos_task_create` NO LONGER WAITS (maintainer, 2026-11-09).** Its timeout bounds
      only the wait for the instance's last incarnation to finish dying, which reads as a bound on
      the create itself. The create answers `-KOS_EAGAIN` while the last incarnation is dying, its
      members, lines or IRQ bindings still retiring, and a creator waits for "this instance can
      start again" through a notification, as `kos_task_watch` reports an end. The smallest surface
      that does it, a new event of the existing watch before a new call, and it folds into "ONE
      CALL TIES ANY EVENT SOURCE TO A NOTIFICATION" when that lands (maintainer: a new API now,
      merged later). Whether that one call lands in M10.7 instead is checked first against its
      design (maintainer, 2026-11-09: "seems a good moment yes, but to check").
      **RULED (maintainer, 2026-11-10: "that's the idea yes"):** the one call lands in M10.7 with
      the task, IRQ and "can start again" events, and `kos_task_watch` and `kos_irq_bind_notify`
      go, rather than a new event on the old call first; every wait takes a deadline of the
      instant type below.
      **RULED (maintainer, 2026-11-09):** "design [the one watch call] to be able to match this
      epoll style later, even if this is not provided in the first version". The shape leaves room for a
      message-waiting source (an endpoint kind, or a notification bound to the thread), several
      timers in one loop, more sources than one notification word holds, and a libc `poll()` on
      top; a wake is reported once and the waiter reads the source's state.

- [ ] **M10.7: THE C++ API CARRIES TIME AS `std::chrono` DURATIONS (maintainer, 2026-11-09).** A C++
      header takes and returns a duration, so its names drop the `_us` suffix (the USB console's
      `max_irq_wait_us` first). Done across the whole C++ surface at once (`kickos/kos.h`, the
      driver headers, the system services), never one function, and "no limit" keeps one spelling
      that converts to `KOS_TIMEOUT_NONE` at the C boundary. The C API gets the same safety from
      one-member structs whose name says the type (maintainer, 2026-11-09), to weigh before it lands:
      one duration type with unit constructors rather than a type per unit, an instant type apart
      from it so a deadline cannot be passed as a timeout, and each struct's passing in registers
      checked on every arch's ABI (the syscall stubs unpack it, so the kernel ABI is unchanged).
      The C++ side defines no clock: `std::chrono` durations convert to the C duration,
      `steady_clock` time points (already KickOS's monotonic clock) to the C instant, and
      `system_clock` only to a wall-time type. The maintainer leans to a 64-bit duration
      (2026-11-09), so no conversion narrows; to settle when the work starts.
      Draft (2026-11-09), each type one `int64_t` of nanoseconds, a C++ duration converting
      rounding up:

      ```
      typedef struct { int64_t ns; } kos_duration;   /* saturating unit constructors */
      typedef struct { int64_t ns; } kos_instant;    /* monotonic, since boot */
      typedef struct { int64_t ns; } kos_walltime;
      #define KOS_FOREVER ((kos_duration){ INT64_MAX })
      ```

      Open: AAPCS32 returns an 8-byte struct through memory (RISC-V and x86_64 in registers,
      Xtensa and RX unchecked), the rounding to the kernel tick as a promise or not, a negative
      duration as an immediate poll or `-KOS_EINVAL`, and the type names against the other public
      headers (no `_t`, which POSIX reserves).

- [ ] **M10.8: THE EXIT RECORD.** Reconcile `roadmap.md`, `docs/reference/architecture.md`,
      `docs/reference/invariants.md` and `STATE.md` against what shipped, and record what the green
      runs do not say.
      **IT OPENS WITH A CROSS-FAMILY AUDIT (maintainer, 2026-11-06).** GPT-6 Astra, through the
      Codex plugin, audits ten areas from a prompt the maintainer keeps. A Claude agent verifies
      each finding before it reaches the maintainer.
      Lines M10.6.9 to M10.6.11 left stale, docs being deferred to the end of M10:
      - invariants.md: `timer-min-delta-guard` calls a deadline immutable, and a wall-clock set
        now moves a realtime one; it fires a past-due deadline at once, where it now fires within
        the minimum delta (maintainer, 2026-11-05: "1. OK"), as Stop 7 of
        `docs/journey/03-sleep.md` must say too; the one-task clauses; the mechanism lines of
        `task-data-starts-from-its-image`, of the cap table's dynamic slot and of the "two
        recursive locks"; the device-window sentence on the LX6, where windows are task-wide.
      - docs/reference/style.md: the `_4` read-modify-write line on armv6m and rxv3 (the user
        atomics library now answers it). porting.md: RX user mode cannot write PSW.I either, the
        restartable-sequence paragraph, the "declared reserve" section, and the M10.6.10 libc
        lines. boards.md: the retired full-newlib knob, the microbit package guard and the
        amp2 THREADS line. bench.md: the terse-diagnostics line. architecture.md: the join
        gate, which any thread of the target's task now passes.
      - The Book: the atomics page's RXv3 row and its link-failure and lock-table paragraphs, and
        the SjLj section. A user-facing line that, on a region board, a sync object on a
        thread's stack faults any other thread that touches it, one of the same task included.
      - A stated limit (maintainer, 2026-11-05): a thread that ends without `kos_exit`, slain or
        killed by a fault, leaves its libc scratch (dtoa lists, conversion buffer, signal table)
        allocated in its task's heap.
      - The journey: stop 08's create comment and stop 09's diagnostics lines.
      - roadmap.md's M10.6.10 row, against what shipped.
      - M10.6.11, by source:
        - the `pe_image.ld` rewrite: `docs/reference/architecture.md:356-358` ("the linker's own
          default layout plus section wildcards"); `docs/design-m10-toolchain.md:638` (`.eh_frame`
          "in the kernel half"; it is now discarded) and 846-847 (`.kosinit`; the constructor
          range is now in `.rdata`); `docs/journey/15-x86_64.md:188` (`.kosinit`); stale before
          it: `docs/reference/invariants.md:745`, `docs/design-m10-kernel-share.md:54-56`,
          `docs/design-m10-toolchain.md:560`;
        - the x86_64 calibration: `docs/journey/15-x86_64.md:342-344`, edited only for a gate;
          it does not describe the TSC brackets or the median;
        - the QEMU hardware clock: `docs/reference/porting.md:1152,2659,2879`, 2929-2935;
          `docs/m2-readiness.md:90-94`; `docs/reference/telemetry.md:85,87,115,379`;
          `docs/journey/01-power-on.md:144,242`, 624-625; `docs/journey/03-sleep.md:131`,
          143-149; `docs/journey/12-armv6m.md:373`;
          `docs/journey/07-interrupt-to-a-driver.md:32,65,121` (on mps2 the free IRQ base is now
          11); `docs/book/the-time-base-a-monotonic-clock-you-can-trust.md:388,409`;
        - the USB CDC console: `docs/reference/console.md`, its USB CDC part around 777-800
          (output held until the host sets line coding after DTR, with 1 s and 5 s fallbacks; a
          TAP image waits up to 10 s for its console to drain); `docs/reference/bench.md:74,90,96`
          (PYBIN is now also needed for USB CDC captures) and 257-258 (identity rows held, sent
          with CRLF);
        - the uaccess witness: `docs/reference/invariants.md:359` is still accurate; it could
          mention the park and move.
        - the teensy41 hang fix: `docs/reference/invariants.md:494-532`,
          `docs/reference/boards.md:609-618`, `docs/design-teensy-rt1062.md:52`;
        - the amp2 fix: `docs/reference/porting.md:1889,2006,2014`, 2042-2043, 2098-2110 and 2337;
          `docs/reference/architecture.md:86`; `docs/design-m10-target.md:503`;
          `docs/reference/invariants.md:399`.
        - the divide-by-zero trap: `docs/journey/12-armv6m.md:423-425` (the v6-M fault report now
          also names the divide hook's UDF); `docs/reference/architecture.md:364` (the
          `kickos_string` object also carries `arch/arm/common/aeabi_div0.cc` on Arm M);
          `docs/reference/invariants.md:323` (the source list of
          `fault-kills-the-unprivileged-thread-task` lacks the divide arms and the sim's SIGFPE
          handler); and no Reference page states the guarantee: a divide by zero kills the thread
          where the core or the ABI traps it, and which arches return a value instead.
      - M10.6.12, by source:
        - the rv32 PMP boot seat: `docs/reference/porting.md:3034` says it reads back the code
          window; it now also turns off every entry the kernel does not program and refuses a
          boot whose entries do not clear.
        - porting.md states no rule that a frame carrying a privilege level or a resume pc sits
          in memory no unprivileged thread can write (an unprivileged thread's first frame on
          its kernel block); rv32imac broke it until M10.6.12.
        - the kernel-only section selectors: `docs/reference/boards.md:520` and
          `docs/design-rp2350.md:33` quote `KEEP(*(.isr_vector))`, now
          `KICKOS_KERNEL_KEEP(.isr_vector)`; no Reference page states that an image section named
          like one only the kernel supplies is refused at link;
        - the armv8a EL0 control registers: `docs/reference/boards.md:201` (the imx8mp EL3
          handover also writes MDCR_EL2); no Reference page states SCTLR_EL1, MDSCR_EL1 and
          PMUSERENR_EL0 written whole, nor what they leave EL0 (wfi, wfe, CTR_EL0, cache
          maintenance by VA and DC ZVA; not DAIF, the PMU or the debug comms channel).
      - M10.6.12, the kernel access to user pages on translating backends: the acquire now takes
        the access and answers no device leaf and no write to a leaf without user write.
        `docs/reference/porting.md`'s acquire-pair section names neither; `invariants.md:223` and
        `docs/reference/ipc-call-reply.md:464` say a late copy re-proves only that the page is
        still mapped; `invariants.md:137` scopes the device-word refusal to backends that do not
        translate, where the range check now refuses one on every backend.
      - M10.6.12, the invariants the maintainer ruled (2026-11-12), for `invariants.md`:
        - New, under "Syscall, privilege & the user/kernel boundary": no return into a thread
          takes privilege state (EXC_RETURN, the stacked PC and xPSR, mstatus, PSW) from memory
          that thread or a sibling can write. Cross-arch: armv7m and armv6m keep EXC_RETURN in
          `ctx.exc_return`, build the SVC trampoline's and the fault exit's frames on the
          thread's kernel block, and save a context off that block at its resting privilege; on
          ARMv6-M the switcher clears a parked frame's IPSR and PC bit 0 before returning through
          it. rv32imac seats an unprivileged thread's first frame on its kernel block and takes
          MPP from the kernel. rxv3 keeps a thread's PC and PSW in its context, never on its
          stack, and a user `brk` or `int` on any slot but the syscall's kills the thread that
          issues it. Each arch's witnesses: `selftest`
          (`parked_frame_exc_return`, `parked_frame_ipsr`, `svc_in_it_block`), `privwindow`,
          `faultsurvive` modes `swtrap`, `forge`, `mppforge` and `wildspin`, `svcfp`.
        - New, beside `wake-from-a-thread-that-will-not-run-again-must-not-switch`, which stays
          as it is: `a-parked-current-takes-no-pass-before-its-own-reschedule`.
          `sched::resched_after_wake` declines the pass while `kernel().current` is BLOCKED, and
          every park site ends its lock span with its own `sched::reschedule` (or `wq_block`),
          never with a wake whose pass would be its switch. Between the park and that reschedule
          the parker's span is atomic with respect to switches on every backend, so
          `kos_task_slay`, parked before it stops the task, marks every member and ends every
          park before its one switch away. Source: `kernel/sched/sched.cc`,
          `kernel/syscall/syscall_ipc.cc` (the call's reply park),
          `tests/unit/schedwake/wake_dying.cc`, `tests/unit/amphold/badge.cc`; and on
          `task-scoped-death-and-a-cancel-that-reaches-every-park`'s source list,
          `tests/unit/taskdeath/slay_walk.cc` and the selftest's `task_slay_walk`.
        - `npriv-banked-on-switch`, narrowed: CONTROL.nPRIV is banked per switch for a context
          saved on its own kernel block, since a thread blocked mid-syscall must resume
          privileged; at `KICKOS_KERNEL_STACKS` 1 a context saved anywhere else is saved at its
          resting privilege, so no privileged context rests in memory a task-mate writes.
        - `amp-user-share-is-roots-reservation`, one clause after "The arena confinement admits
          it and nothing else outside the arena": a thread's stack is never in the share. On a
          region board a caller-named stack meeting it is refused `-KOS_EPERM`
          (`ram_stack_admit`), and under translation a stack needs a pool frame, since the
          kernel resumes a thread through frames on its stack and another node's tasks write the
          share.
        - Unchanged by ruling: `invariants.md:158` keeps `KOS_SYS_DIAG_LED_SET` as it is, and
          arm64 EL0 keeps `SCTLR_EL1` UCT, UCI and DZE set.
      - The boot banner (maintainer, 2026-11-03): its version (the project's 0.5.1) and its text
        are out of date.
      - Every guarantee states its limits beside it (maintainer, 2026-11-05). KickOS provides a
        tool, with its pros and cons, and the integrator alone is responsible for safety, so each
        guarantee in `docs/reference` (invariants.md included) is documented with its worst cases
        and what it does not cover, in the same place: every bound's value, the longest wait, the
        error each failure answers, and what is not protected (for example: a transfer in progress
        is never interrupted; on the ESP32 the kernel is reachable from user code). A reader of a
        guarantee must not need TODO.md or a board page to learn its limits.
      - TODO.md is swept and cleaned (maintainer, 2026-11-05): ticked boxes are removed, not kept
        as history. Before a box goes, any fact still needed (a measurement, a ruling, a limit, a
        rationale another box or a doc points at) moves to its proper home (docs/reference, a
        design note, STATE.md) and every reference to the box is fixed, since git history is not
        an archive a reader follows.
- [ ] **A DEFERRED DELIVERY ON A REGION BACKEND RE-PROVES NOTHING (M10.6.12 audit review,
      2026-11-11).** A translating backend re-proves an out word or a parked receiver's buffer at
      the access, through the acquire. A region backend writes the address the boundary check
      admitted, however late. Not established: whether any route takes a region from a parked
      thread's own set (a retype, a block given back) before its delivery. If one does, the late
      write needs a re-check of the set at the access, or the wait must end with the region.

**DEFERRED UNTIL A CONSUMER NEEDS ONE, AND NOT WORK OF THIS MILESTONE.** The seal, a
create-suspended spawn, and an authority for placing a thread of another task, a dynamic mint and
starting a core. None may be answered with "root" when it comes, nor in a way that suits only the
boot init: a nested init is the consumer most likely to reach them first. The temporal half is out
by ruling and sits in `roadmap.md`'s `Later`.

## The KickOS journey (maintainer, 2026-10-12)

- [ ] **AFTER M10.6.5: A WALK THROUGH THE CODE, FROM POWER-ON TO EACH SYSCALL.** It carries no
      milestone number (maintainer, 2026-10-13): the walk, its PL022 capstone and the docs site land
      as one maintainer PR outside the numbering, as soon as M10.6.5 merges. The Book teaches
      the concepts and the Reference states the contracts; nothing takes a beginner through the
      code itself. It goes in a journey directory beside the Book, written on the cleaned tree (the
      code it cites moves in M10.6.5). The reference walk runs on the `qemu` preset (mps2-an386, Cortex-M4; `f411disco` is
      its silicon twin), so a reader needs no board and can step every stop in gdb.
      - **Shape of a stop:** where you are (a file and a symbol, never a line number), what happens,
        what to look at, a "try it" (a gdb breakpoint and what to inspect), then a link to the Book
        for the why and to the Reference for the contract. No status language, as the Book.
      - **Outline:** setup (build, run under gdb, the tree map); power-on to the first unprivileged
        thread (reset vector, startup, clocks, console, arena, scheduler, timer, MPU on, init and
        its caps, the first switch); the first syscall (trap entry, dispatch, the resolve, return);
        one chapter per syscall traced end to end (sleep, mutex uncontended then contended then PI,
        call/reply, shared memory, an interrupt delivered to a userspace driver, thread spawn and
        caps, a fault and its kill); the MMU on `qemu-arm64` as the same stops showing what
        changes; multicore (the kernel lock, IPIs, per-core queues, AMP); one chapter per arch as a
        difference from the power-on and syscall chapters.
      - **The closing chapter is a real system (maintainer, 2026-10-12):** a driver service and a
        client composed together, from the composition through init's grants, the call, the
        transfer and the reply, to a client fault and a driver death. It runs on `qemu`: add a
        PL022 SPI engine behind the bus-service contract (QEMU models five PL022s on mps2-an386
        and on an505, and the PL022's loopback mode needs no attached device), its service and a
        client app, with an emulator gate; the K64F and XMC4800 services are its silicon twins.
        This also gives the bus-service contract its first emulator witness.
      - **The docs become a site (maintainer, 2026-10-12):** the Book, the Journey and the
        Reference built with Zensical (through uv, from an `mkdocs.yml`; a trial on the tree built
        clean with every heading link resolving once the slugs follow GitHub's, through
        `pymdownx.slugs`). Links into the code point at the repository; pandoc keeps the PDF and
        EPUB from the same sources. Its Markdown starts a list only after a blank line, where
        GitHub's does not: the doc gate refuses a list with no blank line before it. Publishing
        it is the maintainer's call.
      - **It takes over the Book's "Per-ISA guided tour" row**, which has no chapter behind it: the
        row points to the journey, one home.
      - **It is code-synced, drift is a bug.** Its stops are also a gdb breakpoint script the
        `qemu` selftest gate runs, checking each stop is hit in order, so a renamed or reordered
        function turns the gate red.

      **PARTLY:** the walk is in `docs/journey` and the site builds from `mkdocs.yml` with Zensical
      in `.github/workflows/docs.yml`. Still open: the PL022 capstone (chapter 18 walks the K64F
      `k64dspi` service instead), the gdb stop gate, and the Book row in
      `docs/book/the-syscall-path-trap-dispatch-return.md` that still names the Per-ISA guided tour.

## Found by the KickOS journey (2026-10-12)

- [x] **MEASURE FIRST: ABOVE ONE KERNEL CORE, ONE SLEEPER'S DEADLINE FIRES ON EVERY CORE.** It
      carries no milestone number until a measurement argues for it (maintainer, 2026-10-13). The
      sleep queue is
      shared and `ktime_rearm` arms THIS core's timer to its head on every switch, so every core
      that switches while a deadline heads the queue fires at it; the first wakes the sleeper and
      the rest find nothing expired. Measured on the `stress` app (an instrumented `ktime_on_timer`
      counting fires armed for a sleeper that woke none, and whether another core had already
      served that deadline): one core, 0 of 477 such fires; `qemu-arm64-smp`, 525 of 1024, every
      one a deadline another core had served; `qemu-riscv64-smp`, 1003 of 1598; `qemu-x86_64-smp4`,
      1079 of 1715 (1068 served elsewhere). Each is an interrupt, a kernel-lock round and a rearm
      for nothing, growing with the core count. Decide who arms a shared deadline (one owner core,
      a per-core queue keyed by the sleeper's core, or a rearm doorbell when the head changes),
      then re-measure the same scenarios. A witness counts the fires per deadline.
      **TAKEN BY M10.6.12 (maintainer, 2026-11-05):** per-core sleep queues. The design note
      `.session/reports/2026-11-05-percore-sleep-queues.md` is approved with its new invariant.
      **LANDED:** each kernel core keeps its own sleep queue, armed and expired by that core
      alone, and a wall-clock set asks each peer whose queue moved. Re-measured on `stress`: no
      fire is served by another core on any SMP preset, and a four-core run takes about 400
      timer interrupts where it took about 1000.
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

## Later, from the M10.6.10 rulings (maintainer, 2026-10-29)

- [ ] **ONE CALL TIES ANY EVENT SOURCE TO A NOTIFICATION.** Each source has its own call today:
      `kos_task_watch` for a task's end, death and readiness, `kos_irq_bind_notify` for a line. One
      generic watch, its parameters naming the source and the events, would replace them, so a new
      source adds a kind and no new call.
      **RULED (maintainer, 2026-11-05):** deferred to M11 (the driver era), to land with the first
      device-event kind. Design: `.session/reports/2026-11-05-event-source-watch.md`. One watch call
      takes a spec, a source, a notification capability and an argument; a kind is a table row, and
      a program-slot kind is part of the shape. A watch never lends priority.

- [ ] **SHARED LIBRARIES ON MMU BOARDS, WITH DYNAMIC LOADING.** M10.6.10 stores libc once in flash for
      the region and flat boards only; an MMU program keeps libc linked into its own image, since RAM
      does not bind there. Sharing on an MMU board is a shared library mapped into each space, which
      belongs with loading programs from storage; a fixed-address runtime in the boot image would not
      survive dynamic loading.
- [ ] **THE SIM RUNS ON THE KICKOS TOOLCHAIN, NOT GLIBC.** KickOS does not need a sim for itself: the sim
      exists to run a user's payload as on a real board (an emulated EtherCAT network of many slaves
      over shared memory, for one), and it is the simplest way in for a user. Today its user side
      builds with the host compiler on glibc and the host's libstdc++, whose pthread calls block the
      one host thread every KickOS thread runs on, so M10.6.10 declares its threaded C++ arms
      inapplicable there. Move the sim's user side to newlib and the KickOS threads model, so it runs
      the runtime the boards ship; until then every gap against a board is a defect, not a difference.
      The time calls are such a gap (found by the M10.6.10 review): on the sim `clock_gettime`,
      `gettimeofday`, `time` and `clock_settime` are the host's, so a wall clock set through KickOS
      never reaches them, and `abort` ends the host process. A fix that interposes them must still
      pass `CLOCK_MONOTONIC` to the host, which the sim kernel itself reads.

## M11, at its start: code that runs from RAM (maintainer, 2026-11-03)

- [ ] **M11, FIRST: RAM-RESIDENT CODE LIMITS WHAT A NODE OR A SMALL BOARD CAN RUN.** Where the code
      is copied to RAM, each byte of text is a byte the arena loses, so an image's code caps its
      thread pool. The ESP32-C6 AMP nodes link `cxxtest` only on nano newlib, with 1,776 B to spare
      on the HP node (measured 2026-11-03). Code runs from RAM today on two boards. On
      `esp32c6-wroom` (rv32imac) every configuration links the kernel and every image into the
      512 KiB HP SRAM, the LP core's node included, and the flash cache is unused. On
      `esp32-wroom` (Xtensa LX6) the kernel's text is in SRAM1 and every image's in SRAM0, with the
      flash cache off. Every Cortex-M board and `rx72m` execute from flash (`picopi`, `pizero2350`
      and `teensy41` through their XIP caches). The emulated `qemu-riscv` and the MMU boards load
      into DRAM, their only memory. So the concern is not one arch: rv32imac and Xtensa today, and
      any later chip whose loader copies the image to RAM. Two levers:
      - **Uneven AMP node slices.** The C6 `amp2` configuration gives the LP-core node the same
        240 KiB as the HP node, since `KICKOS_AMP_NODE_SHARE` is one value for every node (the
        `pizero2350` and `qemu-arm64` partitions too). A size per node gives the RAM to the node
        that runs the larger images.
      - **Execute code from flash through the chip's cache (XIP)** instead of copying it to RAM,
        leaving RAM to data and stacks. The protection windows over each image's code then cover
        its flash address. On the ESP32 it is the second half of "M10.6.11: ESP32 CODE PAST SRAM0
        AND SRAM1" (`git log -S'ESP32 CODE PAST SRAM0' -- TODO.md`).

- [ ] **M11: API CLEANUP LIST (not bug-driven, deferred by the M10-end rule) (maintainer,
      2026-11-05).** These open boxes, each named by its title:

      - "A JOINER CANNOT READ A THREAD'S EXIT CODE";
      - "`kos_thread_create` returns `-KOS_ENOMEM` for two different failures";
      - "A CLOCK RETUNE IS REFUSED WHILE A DRIVER OWNS THE CONSOLE" and "The CPU/peripheral clock
        coupling is over-generalised";
      - "`getentropy` HAS NO SOURCE ON ANY BOARD", for the sources beyond x86_64;
      - "MEASURE FIRST: A THREAD THAT NEVER USES THE FPU SAVES NO FPU STATE" (lazy FPU on armv8a
        and x86_64);
      - the hardening lists in "M6.4 x86_64: what the `qemu-x86_64` port owes" and "M7.6, the
        i.MX 8M Plus EVK and the per-part predicate";
      - the scheduler micro-optimisations, measure first: "The `sched::wake` residue",
        "`sched::wake`'s guard reads a `kernel().current(me)` that a deferred switch has already
        moved", "Swapping `Thread::privileged` and `Thread::dying`", "MEASURE FIRST: A YIELD WITH
        NOBODY TO YIELD TO TAKES NO KERNEL LOCK" and "THE CONTENDED SEMAPHORE IS SLOWER THAN THE
        KERNEL SEMAPHORE WAS".

- [ ] **M11: WHAT THE M10.6.12 RULINGS MOVED TO M11 (maintainer, 2026-11-05).** "LTO does not
      link, on any board" (maintainer: LTO is itself a bug source, and not every user expects it),
      and these:

      - [ ] **A CREATE WITH NO INSTANCE NAMED WAITS FOR ONE INSTANCE, NOT FOR ANY.** With
            `params->instance` NULL and none startable, `kos_task_create` with a timeout waits out
            the first instance it finds still dying (`decl_wait` in
            `kernel/syscall/syscall_thread.cc`), even when another becomes startable first. A
            nameless create waits until any instance it may start can start.
      - [ ] **SIGNAL DISPOSITIONS ARE PER THREAD.** newlib keeps the signal table in each thread's
            `struct _reent`, so `signal()` sets the calling thread's disposition only, where POSIX
            makes it per process.
      - [ ] **THE K64 UART OVERRUN RECOVERY HAS NO SILICON WITNESS.** `k64uartirq` claims its error
            line and arms `C3.ORIE` since M10.6.12, held by host tests only. No app reads RX on
            frdmk64f (`uartloop` is sim-only), so the witness needs a reader app that reads through
            `KOS_UART_READ` slower than the host sends, and a host sender in the bench tools. The
            capture shows a host flood at 115200 raising `rx_overrun` in `KOS_UART_STATS`; a known
            line sent after the flood received with no TX between the flood and the read, which is
            the recovery without a doorbell; and the console printing on with `irq_spurious` flat.

- [ ] **M12: KICKCAT NEEDS A LARGE UPDATE BEFORE IT IS THE REALITY CHECK (maintainer,
      2026-11-05).** No action before M12.

## M10.6 -- the cleanup pass (maintainer, 2026-10-06)

- [ ] **AFTER M10: WHERE THE PROGRAM MODEL LIVES (maintainer, 2026-10-31: kept as is, may change).**
      The kernel holds it today: declarations read from the image, program lookup by name, instance
      choice, start-once and restart, argv copied from the creator, and the boot image check. For
      it: one party nobody bypasses enforces the declaration, and an on-request start is one
      syscall with no server in its path. Against it: user strings, argv and image headers parsed
      privileged, on the syscall descent the trap red zone prices, and a program loaded later from
      storage would need a second path. The split to weigh: the kernel keeps a generic create with
      explicit parameters bounded by what the caller holds; the init, as the loader, reads the
      declarations and keeps names, instances, start rules and argv, so an on-request start becomes
      IPC to it. Decide with the first loader or storage work.

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
goes with it. Each step's close records its lines split as kernel, arch, userspace, build,
tests, tools and apps, with no ratio (maintainer, 2026-10-09); no gate is written over them.

- [ ] **MOVE `kickos_lx6_fault_report` ONTO THE PANIC STACK (review 2026-11-06).** The
      unhandled-exception reporter runs on the stack that faulted, so it cannot report that
      thread's own stack overflow, and the FAULT class it is priced by holds the lx6 floor at
      1088 above one kernel core (784 on esp32-wroom-smp, 816 on -benchsmp, kprintf into the
      console under the kernel lock). Moved, a stack fault becomes reportable and the floor drops
      back to 1024. A silicon capture of a planted thread-stack fault on esp32-wroom-smp is owed.

- [ ] **M10.6.12: THE LIVE BUGS FROM THE TODO SWEEP, AND PER-CORE SLEEP QUEUES (maintainer,
      2026-11-05; numbered M10.6.11.1 until 2026-11-07: "no need for sub milestone when there is
      only one").** It takes these open boxes, each named by its title:

      - the imxrt1062 bus gate below, never closed after its holder;
      - "A FAILED HANDLE WRITE LEAVES AN UNNAMED CAPABILITY INSTALLED";
      - "A SECOND AUTHORITY NARROW ANSWERS -KOS_EBADF";
      - "A WORD BROADCAST IS QUADRATIC WITH INTERRUPTS MASKED";
      - "THE LOCK-FREE AUTHORITY READ WAS NEVER REVISITED FOR SMP" (a verdict; it may be benign);
      - "THE FULL NEWLIB PROFILE LEAKS A DEAD THREAD'S DTOA LISTS", "`_close` FAILS WITHOUT SETTING
        `errno`" and "THE CONSOLE LOOKS LIKE A SEEKABLE TERMINAL FILE";
      - "`k64uartirq` RX cannot self-recover from an overrun";
      - the `Shared::configured` half of the USB "Still owed" box: it does not clear on unplug;
      - "The RV64 doorbell's INSTRUCTION-side half has no operation at this board's ISA baseline"
        (rv64 without FENCE.I);
      - "`sam3x8e` over-alignment": the compose min-region when the MPU is not driven;
      - "LTO does not link, on any board": first establish whether LTO is a supported option;
      - "M10.6.12: THE BENCH ITEMS M10.6.11 LEFT OPEN" below.

      And these rulings:

      - per-core sleep queues, design `.session/reports/2026-11-05-percore-sleep-queues.md`
        approved with its invariant. It replaces "MEASURE FIRST: ABOVE ONE KERNEL CORE, ONE
        SLEEPER'S DEADLINE FIRES ON EVERY CORE";
      - rv64 answers NOCACHE and DEVICE per physical address: accept both on MMIO, refuse NOCACHE
        on RAM. It settles "NOCACHE and DEVICE are still accepted where no PTE attribute carries
        them";
      - rx72m: delete the below-stack fault test and its 16-byte band now
        that the stub runs on the kernel block, and witness `mpu_fault` and `faultsurvive_ovf` on
        rx72m. It settles the two rxv3 boxes under "Found porting fault isolation to rxv3";
      - ESP32: NOT in M10, and probably never (maintainer, 2026-11-05: "not even sure we want to
        pay the cost for such small isolation"). The design note
        (`.session/reports/2026-11-05-esp32-kernel-user-split.md`) found one user PID gives a fence
        against faults, not a wall: the LX6 has no privilege ring, so a task can rewrite its vector
        base or jump into a PID-0 gate. It would cap user RAM at 128 KiB, re-split the layout and
        turn every system call into a trap, with no fault address on a violation. The ESP32 stays
        flat. It settles "M10.6.9: CHECK WHETHER THE ESP32 CAN SPLIT USER FROM KERNEL";
      - imxrt1062: a 4 GiB no-access MPU background region (maintainer, 2026-11-05: "Agree too"),
        taken only if its cost in per-thread MPU slots allows; that cost is measured first.
        **LANDED:** it costs no slot. The background row replaces the four aperture wraps, and
        OCRAM2, the AIPS peripherals and the flash are given back to privileged code by three
        rows, so the fixed rows go from five to four beside the eight per-thread slots, of the
        part's sixteen. The silicon run on teensy41 is owed.
      - newlib's allocator stays out of an image that never allocates (maintainer, 2026-11-09):
        the exit release of a dead thread's libc scratch links its free path only into an image
        that allocates. **LANDED:** the heap seat arms the release before an image's first
        allocation, so an image that never allocates links no `_malloc_r` or `_free_r`.

      - [x] **imxrt1062: A GRANTED USB1 SLOT STAYS OPEN AT THE BUS BRIDGE AFTER ITS HOLDER.**
            `arch_periph_enable` in `arch/arm/chip/imxrt1062/chip_imxrt1062.cc` clears the
            Supervisor-Protect bit of USB1's AIPSTZ slot, and the chip defines no
            `arch_periph_disable`, so the exit path in `kernel/sched/sched.cc` gets the default
            `-KOS_ENOSYS` and the slot stays open after the holder dies. That breaks
            `dev-window-access-is-thread-scoped-its-lifetime-task-scoped` in
            `docs/reference/invariants.md`. Only mk64f implements the close. The per-thread MPU
            that covers devices limits the reach today.
            **LANDED:** the chip sets USB1's protect bit back on a holder's disable and at its
            death; no other chip's enable opens a bus gate.

      - [x] **ESP32-C6: TASK TEXT RUNS FROM FLASH THROUGH THE CACHE MMU (maintainer,
            2026-11-09).** The M10.6.12 libc merge grew each full-libc image by about 3.3 KiB, and
            `esp32c6-wroom-bench`'s `bench` image stopped linking at its arena assert. Ruling: "C6
            is not a small part in any means ... no execution from flash directly?" The TRM v1.2
            documents no cache MMU register, so its facts may come from Espressif's sources (the
            ESP-IDF headers, the ROM ELF, the bootloader), facts only and written in KickOS style;
            the normal flash boot is preferred over Direct Boot. The isolation bar is the one PMP
            TOR code window over every image's code, item 7 of the M10.6.10 ruling, now over flash.
            **LANDED:** every image's code span, rodata and `.data` template run from flash, mapped
            by `Reset_Handler` before the first template copy, on every C6 preset but the own-image
            AMP pair; the kernel stays in HP SRAM.

- [ ] **M10.6.12: THE BENCH ITEMS M10.6.11 LEFT OPEN (maintainer, 2026-11-08).** M10.6.11 closed
      with these sub-items of "M10.6.11: THE BENCH PASS, BOARD BY BOARD" still open; what it
      witnessed left this file at its merge, and
      `git log -S'THE BENCH PASS, BOARD BY BOARD' -- TODO.md` finds it. A static gate named in an
      item goes once its capture judges it.
      - **The loader witness by device (review 2026-11-09):** the reboot judge now asks for the
        device the capture selected, by serial on the RP boards and by port on teensy41, and M10.6.11's
        loader captures carry only the old kind-on-the-bus record. Retake `rebootdemo` and the
        panic and fault images that end in the loader on picopi, pizero2350 and teensy41; on
        teensy41 this also shows HalfKay comes back on the same port.
      - **frdmk64f:**
        - [ ] A clock-select run (a `KICKOS_CLOCK_RETUNE_TEST` build of `clockretune`) retunes the
              console under its lock and keeps printing.
        - [ ] Owed since M10.5: the SYSMPU user rights narrowed to the core, with no chip address
              for a fetch fault. `rootfault` passes; no capture holds a fetch fault yet.
      - **teensy41:**
        - [ ] A failed CDC bring-up traps (a planted failure). The USB console selftest,
              `usbcdcwit`, `wallclock_usb` and the identity rows passed in M10.6.11.
        - [ ] The plain reset through AIRCR, which `rebootdemo` never reaches since it lands in
              HalfKay.
        - [ ] Owed since M10.5: the LPUART6 RX daisy fix (input on the console's RX pin), and the
              D-cache on in every image.
      - **f411disco:**
        - [ ] `f411spi`'s loopback clause, which needs PA7 jumpered to PA6.
        - [ ] M10.6.7's kernel-lock wait: `mutexbench` on the -bench preset, the kernel's own rows
              over each handoff window beside the user rows.
      - **xmc4800-relax:**
        - [ ] A clock-select run (a `KICKOS_CLOCK_RETUNE_TEST` build of `clockretune`) retunes the
              console under its lock and keeps printing.
        - [ ] `xmcssc` transfers at the slowest clock the bound admits: a word's real receive
              time against `SLOW_WORD_US` (`tests/unit/drvbringup/usic_stall.cc` holds the
              bound's logic, not the engine's margin). M10.6.11 captured it at 921595 Hz only.
      - **picopi:**
        - [ ] The banner at 115200 with the console pins written in their new order and the
              divisors from the hardware read; the 1 MHz watchdog tick (12 cycles, 7 on the ROSC);
              the LED, which needs a human, and the pinmux arms.
        - [ ] A failed CDC bring-up traps (a planted failure). The USB console selftest,
              `usbcdcwit`, `wallclock_usb` and the identity rows passed in M10.6.11.
        - [ ] `bench` ends as the RP2040 boot device (`2e8a:0003`) with no `-D`. The board has no
              -bench preset, so one is owed first; `selftest` ended there in M10.6.11.
        - [ ] The plain reset through the watchdog, which `rebootdemo` never reaches since it
              lands in the boot device.
        - [ ] M10.6.7 on armv6m: `mutexbench` (the restartable sequence's uncontended and contended
              rows), and `ucasrewind` from a `KICKOS_WITNESS_REWIND` build judged by
              `check_rewind.sh`, the first silicon run of the PendSV rewind.
        - [ ] M10.6.12's armv6m half is written and NOT witnessed: no emulator has an armv6m
              privilege ring. `selftest` (`parked_frame_exc_return` verdict 1, `parked_frame_ipsr`
              status 0 through the switcher's IPSR scrub), `faultsurvive` and `pspguard_svc`,
              `pspguard_svcdepth` on the SVC frame built on the kernel block, the resting-privilege
              save and the fault exit's frame on the block.
      - **pizero2350:**
        - [ ] The pinmux arms and the USB console's clock stop wait. The banner at 115200 and the
              wall-clock sleep passed in M10.6.11.
        - [ ] `bench` ends as the RP2350 boot device (`2e8a:000f`) with no `-D`. The board has no
              -bench preset, so one is owed first; `selftest`, the panicking and faulting images
              and `rebootdemo` ended there in M10.6.11.
        - [ ] The plain reset through the ROM's normal reboot, which `rebootdemo` never reaches
              since it lands in the boot device.
        - [ ] M10.6.6's stack limits, red: a capture with the limits restored after SP. Green in
              M10.6.11: `faultsurvive_ovf` killed by STKOF (`CFSR=0x100000`) with main running
              on, `faultsurvive_ovfmpu` ending in the MSTKERR dump, `faultsurvive_off` refused,
              the boot device reached after a panic, and both -amp2 nodes booting.
        - [ ] On -flat: the PMSAv8 regions rounded by the chip file's granule.
        - [ ] On -amp2: the doorbell on the shared protocol, both nodes booting, payload
              and console traffic both ways, the claim's release waking a waiting writer; the peer
              launch, ACCESSCTRL and clock handoff as before; the AMP window cleared by the
              primary alone.
        - [ ] On -amp2: the node that does not own the chip's reset is refused one.
      - **esp32-wroom:**
        - [ ] Timers, sleeps and trace timestamps keep time on the TIMG clock. `wallclock` passed in
              M10.6.11; no capture carries trace timestamps.
        - [ ] The shared ESP console backend and its TX latch, and the UART console selftest
              through the console service loop; a TX burst longer than the FIFO, judged on
              TX-empty dispatches per byte, after which `esp_tx_latch_ack` goes.
        - [ ] `fp_switch` with its preempter holding f0-f15 across a spin: the rounding mode and
              flags stay per thread, red with the control arm planted.
        - [ ] `faultsurvive_off`'s off-stack frame. This flat board builds no `faultsurvive`.
        - [ ] On -smp: a boot row printing ATOMCTL per core and the cross-core wake arms, after
              which `lx6_park_mask`'s ATOMCTL half and `doorbell_generic`'s lx6 clause go. The
              row prints on both cores since M10.6.11.
        - [ ] M10.6.7 above one core: `mutexbench` on -smp, the handoff with holder and waiter on
              different cores, and on -benchsmp its kernel-lock wait.
        - [ ] M10.6.7's SCOMPARE1 bank, red: `user_cas_scompare1` keeps `w` at 0 on the one-core
              preset and on -smp (M10.6.11); owed, each capture red with the `F_SCOMPARE1` save
              and restore taken out of the level-1 frame (`arch/xtensa/chip/esp32/startup.S`).
      - **esp32c6-wroom:**
        - [ ] Sleeps, the tickless deadline and the bench cycle source on the CLINT.
        - [ ] The shared ESP console backend and its TX latch, the refusals through the console
              gate, and the UART console selftest images through the console service loop; a TX
              burst longer than the FIFO, judged as on esp32-wroom.
        - [ ] Every -bench image, its arena at zero slack.
        - [ ] A constructor row reading the clock after an EN reset, after which the `c6_hp`
              image rule goes.
        - [ ] On -amp2: the HP and LP doorbell through the shared body, the ring both ways with no
              lost wake under load, the HP writer's wait for room; the MPU stash; the AMP window
              cleared by the primary alone; the node that does not own the reset is refused one.
        - [ ] `ampping-c6.capture` re-taken: it carries lines no source prints, and its 160 MHz
              line was edited in by hand.
        - [ ] M10.6.7's dummy `sc` at `.Lrestore` (`arch/riscv/rv32imac/switch.S`), which QEMU's
              trap hides by dropping the reservation itself: first record whether the HP core's
              `mret` keeps a reservation. If it does, an arm still to write in which a thread cut
              between its `lr.w` and `sc.w` resumes after only plain stores on the same hart wrote
              the word (the one-core kernel's masked compare-and-swap, a contender setting
              WAITERS) must see that `sc.w` fail, red with the dummy `sc` deleted. If it does not,
              the deletion stays green on this core and the witness is owed to one that keeps the
              reservation; no rv64 silicon is on the bench for `rv64imac/switch.S`'s twin.
      - **rx72m:**
        - [ ] The pin guard, and `mvtipl` in `sysdefault`. `rxdrv`'s console pin refusal passed in
              M10.6.11; no capture carries the system's end.
        - [ ] M10.6.7's rewind witness: `ucasrewind` from a `KICKOS_WITNESS_REWIND` build, judged by
              `tests/integration/check_rewind.sh` (the command is in `docs/reference/bench.md`):
              at least one rewind counted in `_kickos_rx_pendsw` and the word equal to the adds;
              a second capture with the `mov.l r15, 8[r0]` write-back dropped goes red on the word.
      - **A real Cortex-A53** (M10.6.12's EL0 control registers; QEMU resets these clear or does
        not enforce them, so no QEMU arm goes red without them):
        - [ ] The PMUSERENR_EL0 write: `el0_control_bits`' `e0pmu` is green without it.
        - [ ] The imx8mp EL3 handover's MDCR_EL2 write, which keeps EL1's MDSCR_EL1 and
              PMUSERENR_EL0 writes from trapping to EL2.
        - [ ] SCTLR_EL1.SA and SA0: a misaligned SP at EL1 or EL0 faults.
      - **The clocks and the bench itself:**
        - [ ] A capture that crosses a 32-bit wrap of each clock M10.6.11 moved to the overflow
              interrupt:
              `stm32f411`, `imxrt1062` and `rxv3`, and `sam3x8e` once a due unit works.
        - [ ] M10.6.7's owner-death walk measured against the bound its invariant states
              (`mutex-owner-death-reaches-the-next-taker`): one IrqLock section handing over a
              record per waiting slot, on the board with the most thread slots, with the
              owner-death rows M10.6.11 added to `bench`. With it, the end-of-wait scan's bound,
              about S^2 steps under the kernel lock for S thread slots (found by the invariants
              audit, 2026-10-21).

## Parked (maintainer, 2026-11-10)

Out of M10.6.12: the bench items below wait for a new bench or for the driver era, or are set
aside for now.

- [ ] **The USB console across an unplug, a replug and a host suspend: doom until a NEW bench**
      (blocked until a new bench exists; picopi, pizero2350, teensy41). Each board is powered from
      its USB cable, so it needs another supply (picopi through VSYS, teensy41 through VIN with
      the VUSB-VIN pad cut, pizero2350 through its power input) and a human for the cable. With
      `usbcdcwit` or `wallclock_usb` printing: a plain run first, where `irq_wakes` and
      `irq_spurious` stay flat while idle (the RP chips raise a suspend at attach); after an
      unplug of a few seconds and a replug, the stream opens with the identity rows and goes on
      from just after the last line before the unplug, at most one 64-byte packet lost and counted
      in `tx_dropped`; after a host suspend and resume, the output goes on with no identity rows
      and no gap.

- [ ] **blackpill and imx8mp-evk: driver era.**
      - **blackpill** (its probe read 0 V through M10.6.11):
        - [ ] The clock bring-up reaches the PLL, and a panic dump's last line is whole.
        - [ ] `rebootdemo` resets through AIRCR.
      - **imx8mp-evk** (no board on the bench):
        - [ ] The boot, with the boot map's DRAM blocks derived from the layout.
        - [ ] The shared walker giving back the tables an unmap empties, and the selftest's
              granule seat.
        - [ ] The GICv3 refusal path through the console gate.
        - [ ] The alias-sync data-integrity arm.
        - [ ] `rebootdemo` resets through WDOG1.

- [ ] **due (Arduino Due): parked, "forget it for now"** (the unit is faulty,
      `docs/reference/boards.md`).
      - [ ] `rebootdemo` resets through AIRCR.

## The toolchain release (maintainer, 2026-10-04)

- [ ] **RULED (maintainer, 2026-11-05): THE NEXT TOOLCHAIN RELEASE DEFAULTS RISC-V TO ZIFENCEI.**
      The recipe's `--with-arch` default carries Zifencei, as every RISC-V chip's march does since
      M10.6.12.

- [ ] **THE TOOLCHAIN PROBABLY SHOULD NOT REACH A USER AS A CONAN PACKAGE (maintainer,
      2026-10-04).** To use a prebuilt toolchain today a user needs Conan:
      `tools/kickos-toolchain.sh` runs `conan profile detect` and `conan cache restore` on the
      release archive, and the build finds the compiler through the Conan cache. That is heavy for
      someone who only wants to compile for one board. To settle: a plain archive a user unpacks
      and points `KICKOS_TOOLCHAIN` at, with Conan kept for building the toolchain from source and
      in CI if it still earns its place there.

- [ ] **RULING (maintainer, 2026-11-08), FOR LATER: KICKOS'S OWN CODE TRAPS A DIVIDE BY ZERO ON
      RISC-V, AARCH64 AND RX.** The same day's ruling, "trap divide by zero whenever possible",
      already holds where the core or its run-time ABI traps (Arm M, the sim, x86_64, LX6).
      Their divide never traps, so KickOS's own code, the kernel and
      KickOS's userspace, is compiled with GCC's `-fsanitize=integer-divide-by-zero
      -fsanitize-trap=integer-divide-by-zero`; for a user's own apps it stays the user's choice.
      Measured: on rv32 each check is a `bnez` and an `ebreak`, 4 bytes a site, and across the 100
      kernel, arch, system and lib objects of `qemu-riscv` it costs 2 bytes of text in all, since
      GCC proves most divisors non-zero. It covers only code KickOS compiles, not the prebuilt
      newlib and libgcc, and the thread dies on a breakpoint, not on a divide. The witnesses to
      update when it lands: `faultsurvive_div0` and `faultsurvive_ldiv0` expect `returned:-1`,
      `returned:0` or `returned:any` on those arches today
      (`tests/integration/gates/faultsurvive.cmake`).

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

## M7.7 -- AMP

S7 is landed: `qemu-arm64` ships an AMP posture as a preset, the instance index comes from the
core identity, and the shared window's validation is driven by forged publications rather than
argued. What is left here is what the step deliberately did not close, and one ruling it hands
back.

- [ ] **THE PER-NODE COUNTERS ARE RELAXED ATOMICS AND THE ROWS ARE STILL SHARED RAM.** One
      writer per row makes the load/store pair enough for a torn read, which is all that was
      claimed. It is not protection: a peer kernel writes any row it likes, and the labelling
      below is the whole of the answer until partitioning lands.
- [ ] **AMP IS PROTOCOL SCAFFOLDING AND NOT A HOSTILE-KERNEL BOUNDARY, and the window's header
      says so.** The mint sits outside the shared window, but every node maps the same writable
      kernel RAM, so a compromised peer kernel rewrites validation state and every other node's
      kernel data. The label stands until per-node memory partitioning lands, which is the
      partition layout the contract leaves deliberately open.
- [ ] **THE PORT MINT IS SEATED FOR EVERY NODE BY EACH IMAGE'S KERNEL INIT, FROM ONE PARTITION LIST.** That is the
      static configuration act the freeze names, and it is deliberately not an answer to who
      may mint one: the mask is written once before any node can be poked and read-only after,
      and it sits OUTSIDE the window so a far side able to write it cannot validate itself.
- [ ] **THE NODE BUILT HERE IS TRANSLATING-MODEL AMP, AND THE CV1800B, A NAMED AMP PART, IS
      MIXED-MODEL.** Both sides of this posture have an MMU. The CV1800B does not: its main C906
      has one and its companion C906 has none, in the datasheet's own words, which is also why
      that part fails the predicate's symmetry requirement and is an AMP part BY THE PREDICATE
      rather than by judgement. Nothing in the tree witnesses a translating node beside a region
      node, and that pairing is a different port from the one just written.
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
- [ ] **NO BAUD RATE IS PROGRAMMED ON THE CONSOLE.** The divider runs off a clock this port
      configures nothing else in, and the model ignores it entirely. Real hardware needs it, and it
      arrives with the clock tree rather than on its own.
- [ ] **THE EL1-ENTRY PATH AND THE EL2 REFUSAL HAVE NO VEHICLE.** The machine always hands over at
      EL3, so the branch that accepts a firmware handover at EL1 is never taken and the EL2 refusal
      beside it has never fired. Both are one branch each and were written rather than left out, an
      unnamed handover being worse than a named refusal.
- [ ] **`ICC_SGI1R_EL1.RS` IS IMPLEMENTED AND UNEXERCISED.** Every part in scope has affinity 0
      below 16, so the range selector is always zero and the send emits one write per cluster. A core
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

## Retired from the M4.7.2 review backlog (triaged 2026-08-06)

M4.7.2, .3, .5 and .6 closed most of the review backlog without the entries being updated. What
survives is below; everything else was re-verified fixed against tree `82fa51f`.

- [ ] **M10.8: `docs/reference/architecture.md` has never had a full-document correctness audit.** The
      M4.7.x edits corrected only the rows a grep surfaced; the rest is unreviewed. It is the one
      reference doc no reviewer covered in full -- the agent assigned to it died without
      reporting -- and the 2026-07-29 codebase sweep that used to back it up has been deleted
      from the tree, so nothing banked covers this file at all.

## Found during the M4.7.5 configuration-mechanism work (triaged 2026-08-06)

The whole fleet is on Kconfig now, so anything that once read "scoped to a crossed board" applies
to all 20.

- [ ] **A board's provisioning is repeated once per variant and nothing compares the copies.** The
      corpus is `git ls-files '*/configs/*/defconfig'`, 52 defconfigs over 20 boards at the time of
      writing (20 `base`, 14 `flat`, 13 `st`, 3 `bench`, 2 `telem`), each a
      COMPLETE statement rather than a delta on `base` -- which is the Kconfig model and what
      `savedefconfig` writes back. A board with `base` and `flat` states
      `KICKOS_USER_STACK_SIZE` twice and an edit to one is silent in the other.
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
      prompted `choice` at `Kconfig:1002-1018`, a real choice on a board with two transports and a
      fact on a board with one, with nothing distinguishing them.
- [ ] **Decide whether the per-app build stamp should be reproducible.** It folds `__DATE__` and
      `__TIME__` in an app TU (`user/include/kickos/app.h`), so its CODE size varied between two
      builds of an identical tree (measured at 0x8c, 0x90 and 0x94) and every later address shifted
      with it. R2.2 turned the app's half into DATA and moved the
      reformatting into `kmain`; the flash-fix pass turned that half into a `const`
      `kickos_app_stamp` and deleted the reformat, so the app's half is now a `.rodata`
      string and no code at all, and the code-size variation is gone. The timestamp is not. It
      exists to answer "did the APP change or was the image relinked", which a content hash would
      answer without perturbing code size. While it stands, **"byte-identical
      image" is not a claim this tree can make**, which `docs/reference/boards.md:2814` now states
      correctly ("Tree identity is the test, not hash identity").
## Found auditing the panic and fault scope (2026-08-06)

Two audits are in flight on this, both gitignored spikes in the main checkout
(`.session/spikes/audit-panic-scope-syscall.md`, `.session/spikes/audit-panic-scope-faults.md`).
The items below are the parts that belong in tracked history whatever those audits conclude.

## Found by the M4.7.9 ten-angle review (2026-08-07)

Both items were measured on `23b9abb` and neither is a defect this milestone introduced. Both are
scheduler and console core-path work, so neither rides M4.7.9. The capture behind both is
archived `M4.7.9_teardown_latency_meas.md`.

- [ ] **STILL OWED by the fix above, and it is silicon: narrowing the guard puts MORE traffic
      through the preemptible window between a fault redirect and its stub**, so the fault-record
      race gets more likely, not less. `fault-record-is-printed-only-by-its-owner` is the invariant
      that carries the weight now, and no host gate can discharge it. Wants an enforcing board with
      a fault arm under teardown pressure.

## Left out of the M4.7.9 diagnostic catalogue, on purpose (2026-08-07)

Both were considered while `include/kickos/diag.h` was built and both were declined for a reason,
not deferred for lack of time. Recorded so the next reader does not re-derive the reasoning and
"finish the job".

## M4.7.4 -- delete the legacy management (nothing is released before the ABI freeze)

KickOS is unreleased and will not ship before the ABI-freeze milestone, so
**there is no backward compatibility to manage**. Anything that exists only because something else
USED to exist is cost: it must be kept in sync, it reads as a supported path, and it makes a deleted
thing look alive. `roadmap.md`'s ledger
carries the number and the class definition.

**OWED AT THE FREEZE ITSELF, and nothing else in the tree records it: a full doc and comment
sync pass.** The instruments that keep prose honest are deliberately partial and stay that way.
`check_doc_names.sh` validates a PATH, every UPPERCASE-prefixed IDENTIFIER and every
`kickos_`, `kos_` and `arch_` name, so a dead spelling fails the build. It cannot say whether the
sentence around a live name is still true, and a function name outside those families is outside
its corpus. It is a helper for keeping docs roughly in step, not a proof, and a checker that
cries wolf gets disabled. The answer to the rest is a deliberate pass at the freeze, when the
ABI stops moving and prose written against a moving target can be reconciled once against a
fixed one, rather than a gate that tries to hold the line continuously against a target that is
still moving.

**The sweep has been RUN.** M4.7.3 audited all four surfaces and banked the inventory at
`.session/spikes/legacy-audit.md` (gitignored, main checkout only). M4.7.4 is execution against the
rows below, not discovery. Every row was re-verified at `6f2eb55`.

**Class 1 (tombstones) and class 2 (fallbacks reachable only from a broken build) are EMPTY.** The
one instance of each was the root width macro, and both died with M4.7.3's generated header; the
supply assert that commit orphaned was deleted in the same milestone. An enumeration of every
`if(DEFINED)` (11), `message(FATAL_ERROR)` (50) and `#error`/`#warning` (17) site found no other
instance -- each names something that still exists.

**`.github/workflows/*.yml` is CLEAN end to end** -- the one surface of the four with no residue.
Every symbol, ctest case name, doc path and preset name it references resolves.

**The rule to apply:** delete it if its only justification is history. Keep a guard only when it
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

- **A shutdown's reboot outcome shares `kos_shutdown`'s authority** (`AUTH_SYSTEM`), rather than
  taking a capability at a reserved index, and the bootloader outcome takes `AUTH_BOOTLOADER`
  beside it: an authority bit is TCB state and costs no index. **LANDED** in M10.6.5 as
  `kos_shutdown`'s outcome argument.
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
WAS handled by a two-board `-Os` block applied under the self-test knob, since DELETED and
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
   `commit 270b6fa` and the XMC service list came off the root-MMIO
   refusal list, which was emptied then and has since been deleted outright.
6. **`stm32f103` `arch_mpu_min_region()` override.**
7. ~~**Re-point `kernel_ctor_placement` at the `cxxtest` ELF.**~~ -- gone with the gate.
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
      no reason to keep the definition (measured on the since-retired bluepill-c8; the defect record is
      `docs/design-flash-footprint.md`, *LTO does not link*). So LTO is not an available footprint
      recovery today. It is not on the `-Os` path
      N16 needs, so it gates nothing -- filed so it is not rediscovered. **Record only: no fix
      attempted.**
      **RULED (maintainer, 2026-11-05):** M11 settles whether LTO is a supported option: LTO is
      itself a bug source, and not every user expects it.
- [ ] **`kickos_core` no longer carries the archive group.** c539d1c moved the RESCAN group onto
      the leaves, and it rides `KickOS::kernel`, the one leaf left. A consumer linking
      `kickos_core` DIRECTLY gets usage requirements but no archives. The
      documented contract already says consumers link a leaf and never core, and both
      out-of-tree export gates pass -- but core is still in the export set, so the contract is
      now load-bearing where it used to be advice. Either state it in the exported package or
      make linking core alone a configure-time error.
## M4.5.1 -- CI hygiene (2026-07-26)

- [ ] **Pin GitHub Actions to commit SHAs**, not moving tags -- a tag is a supply-chain seam
      controlled by someone else.

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

Blockers and limits:

- **One service bring-up body used to poke MMIO directly from root -- CLOSED by M4.5.6's
  `arch_periph_reg_write` seam, and `xmc_spi0_start` now contains zero register access.** The
  consequence is discharged too: the XMC service list came OFF the root-MMIO refusal
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
  authority cap type was deleted and the word moved to `Thread::authority`, and M10.6.4 dropped
  the clock index, so the reserved range is `KOS_CAP_STDOUT = 0` only, with
  `KICKOS_CAP_FIRST_DYNAMIC` at 1 (`system/include/kickos/sys/cap_index.h`,
  `cmake/cap_geometry.cmake`); see the reboot-outcome bullet above and the
  SUPERSEDED-in-mechanism note below. **The five-bit authority ceiling is gone**: the width is
  `kos_thread_params::authority`'s 32 bits rather than a capability entry.
- **Delegation packing no longer collides with reserved names** -- spawn delegation puts cap *i*
  at child index *i+1*, which since M10.6.4 is the first dynamic slot onward: the clock index is
  gone, and `abi.h` asserts the default placement starts at or above `KOS_CAP_FIRST_DYNAMIC`.
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
      **What stands:** `bluepill-c8-st` and `f302nucleo-st`, both since retired. The deleted two-board holding measure
      already built those two `-Os`, and their `.text` is byte-identical under `MinSizeRel`.
      **What the move buys, which changes what a witness is worth:** a board's `-st` kernel and its
      non-`-st` kernel differed by **13,505 bytes** as shipped, so an `-st` witness never
      transferred to the shipped image at all. Both now build `-Os` and differ by under 200 bytes
      -- the flag's genuine content (`arch_reboot`, `arch_irq_inject`, the MPU probe,
      `irq_spurious_count`) -- so an `-st` witness finally does transfer.
      **The same pass clears this milestone's silicon debt.** Each item is unwitnessed for its own
      reason and none of them justifies a separate bench trip:
        - The UART-FIFO drain on the reboot path (`arch_console_flush_sync` before `arch_reboot`,
          `kernel/syscall/syscall.cc:389`). Only `mk64f` and `xmc4800` implement the seam and
          neither has an emulator gate, so the sim and QEMU gates exercise only the no-op fallback
          (`arch/seam_defaults.cmake`) -- the truncation fix itself has never
          run against a real
          UART.
        - `dev_window_exclusive`: it postdates every silicon capture, so no chip has ever run
          it. Already recorded under the five-apps DEV-window item below.
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
      `kernel/syscall/syscall_thread.cc:1077` is "thread pool exhausted" and `:1151` is "stack arena
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
      it spliced mid-token, flushed only by the bootloader handover of that time. On an
      image that halts instead of rebooting those 419 bytes are lost outright. The drain is therefore
      live and load-bearing on RP2350 even though it is dead code on the sim. Captures were taken
      from a dirty tree; the exact diff is pinned at `.session/m458-silicon/pzdrain-tree.diff`.
      Two claims in the tree overstate this and are owed a fix: `tests/integration/check_fault_dump.sh` (its
      header) and `user/apps/common/fault/main.cc` (its header) both say the marker catches a dump
      lost into an armed ring. On the sim it cannot. What `fault_dump` really covers is the
      `RECLAIMED`-to-polled routing.
      **PARTLY:** a host case now drains a non-empty ring at the panic (`ConsolePanicRace` in
      `tests/unit/consoleown/panic_race.cc` queues a line with the drain held off and needs it ahead
      of the panic's line). The two header claims are still owed.

**Half two -- remove the weak-symbol seam mechanism. COMPLETE** (2026-07-31, developed as an
isolated tip commit, merged in `dde73ca`). Outcome first, then the reasoning that chose it:

- **33 `__attribute__((weak))` definitions removed.** 32 were visible to a plain-fleet `nm` sweep;
  the 33rd, `SystemCoreClock`, was weak only under the in-kernel bench, which is why the sweep missed
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
- **`kickos_app_stamp`** -- hand-rolled C vague linkage, not a seam: `user/include/kickos/app.h`
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

### What the teensy41 bench measured, and the two defects it exposed

- [ ] **`bench` DIES ON teensy41 BEFORE ITS SWITCH AND IRQ SECTIONS.** Both runs end with
      `=== THREAD FAULT === thread 'root' killed`, `CFSR=0x82` (DACCVIOL plus MMARVALID),
      `ADDR=0x20200038`, which is OCRAM2. The app's own header advertises context-switch
      cost and IRQ-entry latency; neither ever prints. Not fallout from the MPU enable fix:
      `imxrt1062` is the one chip that calls `kickos_arm_mpu_fixed_init`, so it enforced
      before and after. So the D-cache figures above cover call/reply ONLY.
- [ ] **A call/reply round-trip is about 12,700 cycles at 396 MHz and nothing explains it.**
      It was about 24,000: 61,124 ns with the cache ON, flat from 16 B to 256 B, so it is
      per-round-trip overhead rather than copy cost: two switches, each with a full MPU reprogram.
      With the flash read in quad I/O (M10.6.11) an 8-byte round trip takes 32.2 us, against
      64.7 us before it. `docs/reference/bench.md` records call/reply figures on other boards to
      compare against.

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
`thread_join`, `slice_preempts_every_core`, `threads_reach_every_core`, `migrate_running`, and
`sleep_order`, corrected above. Four AMP arms listed here were deleted in M10.6.4, their claims
held by the `amp_hold` suite.

## Named by a commit that fixed something else, and never filed anywhere

Each of these was stated as a known-and-not-fixed caveat in the message of a commit that landed a
different fix. Filing them so they stop living only in `git log`.

- [x] **`k64uartirq` RX cannot self-recover from an overrun** (`372e7b4`). `OR` blocks `RDRF`, and
      IRQ 32 (UART0 error) is unclaimed, so recovery happens only on the next TX doorbell. Design
      section 7.7 predicts this but says "TX or RX wake"; there is no RX arm, so it is TX-only.
      Invisible on a TX-heavy console, which is exactly why it will surface on the first RX-heavy
      one.
      **LANDED:** the driver claims `err` (IRQ 32) beside `rxtx` and open arms `C3.ORIE`, so an
      overrun left with `RDRF` clear wakes the IRQ thread, whose read clears it, counts
      `rx_overrun` and keeps the byte the overrun did not touch; close disarms it.
- [ ] **The linker's own `.gnu.warning` output is ignored as noise** (`16662b2`). The missing
      `__getreent` that made every Xtensa stdio call crash was announced by the linker and read past.
      Recommendation as recorded: a `-Wl,--fatal-warnings` gate, or a CI grep for linker warnings.
      Not added.
- [ ] **The RX72M GROUPBL0 demux is verified correct against the manual and has NO consumer**
      (`a19e484`, `09f02b1`). `rxsci` deliberately uses only the dedicated TXI/RXI vectors, so the
      group path is unexercised even in QEMU. Correct-by-reading is the weakest evidence class this
      project accepts.
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
      write with `TIE` armed IS an interrupt. Use the P80/PORT8 LED, which shares nothing with
      SCI6 (`.session/logs/m461-rx-led.log`; `rx72m` no longer backs `arch_diag_led_set`), not a
      UART marker. (`RXSCI_TRACE` and
      its `'P'` push are GONE as of the generic-service rework; the warning stands for any
      replacement probe.) The one measurement that settles it: `Shared::stats` already carries
      `irq_wakes`, `tx_bytes` and `irq_spurious`, readable over the endpoint with `KOS_UART_STATS`.
      Run the reverted control, which now reproduces on demand, and read the three counters at the
      stop. Run the `RXSCI_NO_RX=1` control alongside, as `d2804ce` did, so the reading cannot be
      misattributed to RXI6.
      **A reproducible failing image is the asset this leaves behind**: build the tip with the
      `if (n == 0)` arm removed from `rxsci`'s service loop and the stop returns, every time.
      That loop is now the shared `irq_pass` (`user/src/uart_service_dev.cc`), where the
      `have == 0` call does that arm's job.
- [ ] **The TX drain loop re-reads `SSR` immediately after a posted `TDR` write**
      (`kos_uart_write` in `system/driver/rx72m/uart_sci.cc`). The
      same file's `tie_set` documents, with manual citations, that an RX I/O write is
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
      `cmake/kickos.cmake:20` derives `KICKOS_BOARDS_DIR` from `<repo>/boards` with
      `get_filename_component` -- no cache variable, no override -- and
      `kickos_load_board_descriptor` (`cmake/kickos.cmake:79-102`) has exactly three outcomes: an
      in-tree `boards/<board>/board.cmake`, or, only where `KICKOS_IN_TREE` is FALSE, the single
      board the installed package was built for, or `FATAL_ERROR`. The provisioning path
      no longer looks for a board directory at all: M4.7.5 deleted the per-board headers, so the
      values come from `boards/<board>/configs/<variant>/defconfig` and the only in-tree search
      left is the chip's own include dir. That does not change the conclusion here, because a
      consumer still cannot supply a board without editing the KickOS tree or carrying a patch
      against a vendored copy, while `docs/book/overview-and-principles.md` says porters
      "implement the small arch/chip seam described above, not a kernel restructure". Shape of
      the fix: make the boards search path a LIST a consumer can extend, and let the board
      include-dir lookup search the same list. NOT M4.7 scope -- recorded so it does not
      evaporate. Read directly.
- [ ] **The concurrent capability-teardown path is never exercised.**
      `kernel/include/kickos/cap.h` once stated that an RR slice expiring in `sched::tick_rr` is
      the only thing that switches a dying thread out at a chunk boundary, and so the only way two
      threads are ever inside `cap_teardown` at once. Instrumented counters put that path at **zero
      hits** across the whole suite, including with the quantum cut to 25 us. Forced (a spin in the
      chunk gap plus every churner made RR) it takes 10402 hits with 7 concurrent sweeps and the
      suite still passes, so the design appears sound and nothing guards it against regression. That
      leaves `teardown_depth` and `cap_teardown_active()` (`kernel/syscall/cap.cc`, bumped and
      decremented in `cap_teardown`)
      and the deferred console-death reclaim that reads it (`kernel/sched/sched.cc`) untested by
      anything in-tree. A restructuring that removes the chunked window would DELETE the question
      rather than answer it, which is worth deciding deliberately rather than by side effect.
      **Measured by a subagent with instrumented counters -- the zero-hit figure and the forced-path
      figure are both its numbers, worth re-deriving before acting on them.**
      **NARROWED by `tests/unit/capsweep/`**: the host gate now drives one sweep into another and
      pins `teardown_depth` as a COUNT plus the deferred console reclaim that reads it, so what is
      left of this entry is the ON-TARGET hit count, which is still zero. A restructuring that
      removes the chunked window is the S5 mutant of
      `docs/design-m4.8.2-host-unit-tests.md` section 8.6 and now fails a named arm.

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

- [ ] **The sim's `arch_console_reclaim_window` returns the fake PV register block whenever it is mapped.** The
      sim console's wire is host fd 1 and its reclaim is a no-op, so this is a modelling statement
      -- "if a sim console driver held registers, these are the registers" -- not a hardware fact.
      True in the only sense the sim can make it true (that block is the only DEV window the host
      admits), and it is what lets case 3 exist at all, but it is the softest part of the gate.
- [ ] **`SIMCON_WIN_BASES` is a THIRD copy of `SIM_PVREG_BASES`** (`arch/sim/sim.cc`, the selftest, and now
      `system/driver/sim/simcon/simcon.cc`), each carrying a "must equal" comment and no check. Following the
      existing precedent rather than fixing it was the right call mid-change; fixing it is owed.

### Found by the adversarial review of the cleanup plan (2026-08-03)

The plan was reviewed before execution; it killed three items and found a live bug the plan
never touched. What was FIXED is in the commit. What was found and NOT fixed:

- [ ] **Grant span and reclaim span are not required to be equal**, and a future chip may need
      them different. `console.cc` tests RANGE OVERLAP, not equality, so welding both to one
      constant would assert an invariant the code does not have.
### Recorded -- reported by an angle, NOT verified by me

- [ ] **RX72M is said to have lost plain-vector dispatch**, and **`kos_irq_discard` is said to be a
      no-op on three backends**, and **RXv3 is said to write `IR` on a LEVEL line**. Each is
      plausible and each is silicon-gated; none was reproduced this pass. Check them on the bench
      before acting.

## M4.6.2 USB CDC-ACM: WITNESSED on pizero2350 silicon (2026-08-01)

The driver landed at `9832416` unrun. It now works end to end on an RP2350. Captures
`.session/logs/m462-*`.

- [ ] **Still owed**: the production (non-diag) service list on a pure-stdio app, bulk OUT (host
      to device) which nothing has exercised, and `teensy41`, whose backend
      (`system/driver/imxrt1062/rt1062usb/`) is built but uncaptured on this tree, no longer a seam or a
      half-built backend. Note `Shared::configured` does not clear on unplug -- no backend arms a
      disconnect or suspend source -- so a host that vanishes without a later bus reset leaves it
      reading 1. Wiring that needs matching resume handling and is bench-gated, not blind.
      **LANDED (the `configured` half, M10.6.12):** the field is gone; every USB backend reports
      the bus's suspend state, an unplug included, and bulk IN runs only while a configuration is
      selected and the bus is awake, so an unplug holds the ring and the replug opens with the
      identity rows. Silicon unwitnessed.

## The two sharpest IRQ-driver silicon questions, answered from the manuals (2026-08-01)

Both were flagged as the highest-risk unknowns behind the five unrun `*_uartirq` drivers. Verified
against the TRMs in the local reference set, not against HAL headers or the web.

- [ ] **The RX group-mask path has NO in-tree consumer and has never been exercised, even in QEMU.**
      `rxsci` deliberately does not use `TEI6`/`ERI6` (`system/driver/rx72m/rxsci/rxsci.h`), which are the only GROUPBL0
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

**Do not re-derive the old hypothesis list.** Console-broken, ring-flush, reclaim-hang, vector
routing, stack exhaustion and `KICKOS_POLL_SPIN_MAX` are all now moot for this item: they describe a
panic path that a clean boot never enters. `arch_console_reclaim` on this chip is the empty default
(`arch/seam_defaults.cmake`; only `mk64f`, `xmc4800` and `esp32` define a body),
so it could never have hung -- that candidate died on inspection, before the bench.

## M4.6.1 selftest bench pass -- `irq_claim_gate` and `irq_reclaim` on silicon (2026-08-01)

The first silicon witness of the two IRQ-capability cases M4.6.1 added. Default posture on every
board (**no** service-list selection override -- the existing console, not the unrun `*_uartirq`
drivers), the self-test knob on, `MinSizeRel`. Captures in `.session/logs/m461-*-selftest*.log`.

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

## Found while ruling the XMC console seam (2026-07-31)

Both came out of an RM pass for M4.6.1's `TBIEN` question and neither is about the UART. Recorded
here because they are pre-existing isolation facts, not things that pass created.

## Found during the M4.5.2 stage-2 flip work (2026-07-28/29)

- [~] **`f411spi` cannot run under the flip: its bring-up shim writes MMIO from `main`. ADDRESSED by
      stage 3, silicon-unwitnessed.** The `stm32f411` `arch_periph_enable` backend covers the SPI1
      clock gate and the pinmux encoding covers `PE3`, but `frdmk64f` was the only board on the bench
      for stage 3, so the `f411disco` run is bench debt. Found 2026-07-29 while flipping `f411disco`, and witnessed
      rather than inferred -- the app faults MemManage on the first store of `main`
      (`RCC_AHB1ENR` @ `0x40023830`, `CFSR=0x82`, `MMFAR=0x40023830`), before it ever spawns the
      unprivileged driver that holds the 32 B SPI1 grant. Same shape as `c6blink` and `rxdrv` before
      their windows were reworked: the escalation surfaces (RCC clock-enable, GPIOA/GPIOE mux) are
      deliberately kept out of the driver's window, which is exactly why they need kernel mediation
      instead of a wider grant. NOT a flip blocker -- it is a diagnostic
      app, and the stage-2 gate is `selftest` +
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
      **PARTLY:** `cxxtest`, `libcmix`, `libcrace`, `libc_exit`, `errnoprobe` and `tlsprobe` now check
      libstdc++, newlib, the heap and errno as gated apps (`tests/integration/gates/`), on the emulated
      boards only. The f302nucleo board and its presets are deleted.

- [x] **`sam3x8e` over-alignment: PARKED on hardware absence, not open.** The chip HAS an MPU on
      silicon (Atmel SAM3X/SAM3A datasheet, Cortex-M3 revision 2.0) but `platform/sam3x8e/chip.yaml`
      marks the unit `driven: false`, so it builds `KICKOS_HAVE_MPU=0` while `arch_mpu_min_region()`
      still answers the chip file's granule of 32 (`region_rule` in `tools/compose/kickos_compose/chip.py`
      does not read `driven`), costing 3,808 bytes
      of measured over-alignment on a part that enforces nothing. It is the third member of the class
      `stm32f103` and `stm32f302` just left, both of which overrode to 0 in their chip files.
      **PARKED by the maintainer for a concrete
      reason: the physical Arduino Due unit is dead** (`docs/reference/boards.md`), so nothing on this
      chip can ever be witnessed -- do not pick it up expecting to validate it. The class itself is
      handled by the region-encoding item under M4.5.5 above, which is where a third encoding mode
      would land.
      **LANDED:** `region_rule` reads no unit the build does not drive, so sam3x8e and imx8mp's M7
      answer a region granule of 0.
- [ ] **About 270 `path:N` doc citations cannot be verified by any gate, and two of two spot-checks
      had drifted -- NEEDS A CONVENTION DECISION.** `tests/static/check_doc_names.sh` (landed, deliberately
      not wired into CTest) says so itself at `:57-61`: it strips the `:N` and never checks it,
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
      **PARTLY:** `xmc4800-relax` now carries `CONFIG_CONSOLE_BOTH=y` in its base, flat and bench
      defconfigs (`boards/xmc4800-relax/configs/base/defconfig`); `frdmk64f` still has one transport.

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
- [ ] **`f411spi` lost its high-speed slew configuration on `PA5`/`PA6`/`PA7`**, because the pinmux
      encoding field reaches `MODER`/`AFR`/`BSRR` but not `OSPEEDR` or `PUPDR`, so those pins run at
      the reset-default low-speed slew. `BR=/64` is ~1.3 MHz (84 MHz APB2 / 64, arithmetic from the
      tree); that the default slew carries that rate is **engineering judgement, pending a DS9716
      check** -- no line in this tree supports it, unlike the other electrical claims here. Worth
      reopening if a faster `BR` is ever wanted on that bus, which is what would need `OSPEEDR` back
      -- via the encoding, not a root MMIO write.
- [ ] **The CPU/peripheral clock coupling is over-generalised, and the veto should be a notification.
      Owner: M4.6**, and a CPU governor depends on it. `cpu_clock_set` refuses outright while a
      userspace driver owns the console (`kernel/time/clock_select.cc`), because the kernel cannot
      re-derive a baud it no longer owns. That generalises from a biased sample: exactly **two** chips
      implement `arch_cpu_clock_set` and both are coupled (`chip_xmc4800.cc` fPERIPH = fCPU/2;
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
      though not via `rebootdemo`: the picopi USB CDC console runs record that time's
      shutdown-to-bootloader knob surviving the fault path (`kickos_terminate` reached
      `arch_reboot`), so a faulting picopi image returned itself to BOOTSEL/UF2. The Teensy path
      remains the least certain of the three: it is not vendor-documented, and on non-Teensy
      RT1062 hardware the `bkpt` faults instead.

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
      **PARTLY:** the micro-benchmark is the `bench` app's call/reply sweep, and its measured fastpath
      constraints are in `docs/design-m5-ipc-fastpath.md`. No survey or strategy recommendation is recorded.

## M1 -- clocks (fleet audit 2026-07-09; detail in archived `M1_state.md`)

Every board's timing math is ACCURATE (no ESP32-C6-class constant bug survived the
audit). Remaining work is boards that never raise their PLL, so they run far below
capability and their benchmarks reflect a slow core. Each fix = raise PLL **and**
update `SystemCoreClock` in the same step so the ns<->tick math stays coherent.

- [ ] *(optional perf)* STM32F411 84 -> 96/100 -- deliberate sweet-spot today; only if we
      want the true ceiling. F302 is HW-capped (Nucleo has no HSE crystal);
      K64F/RX72M/F103 already at max; ESP32/RP2040/XMC now at max (silicon-validated).

## M1 -- hardware validation (batch when units are connected)

- micro:bit / nRF51 -- **QEMU-only; silicon bring-up not planned.** The nRF51 is discontinued
      (no silicon obtainable), so it stays an armv6m QEMU vehicle (`-M microbit`). A real-silicon
      port would also have needed an **RTC-based timer** (the nRF51 M0 has no SysTick).
- [ ] Panic/console review HW-checklist: RP2040 PL011 `TXRIS`-at-rest with FEN=0;
      ESP32 UART FIFO DPORT-vs-AHB alias; RX72M `SCR.TIE`-while-`TDRE` fires TXI. (All
      flagged HW-unverified in-code.)
      **PARTLY:** the RX72M question is answered from the UM in `arch/rx/chip/rx72m/chip_rx72m.cc`
      (setting TIE alone raises no TXI, so the ring's prime writes the first byte). The RP2040 prime
      is still marked HW-unverified in `arch/arm/chip/rp2xxx/chip_rp2xxx.cc`, and the ESP32 console
      still writes its FIFO through the DPORT address (`platform/esp32/chip.yaml`).

## M1 -- fleet parity (audit 2026-07-09)

Capability audit across all arch/chip. Fleet is broadly uniform (every arch has a real
console, tickless timer, fault-register dump, inject-driven IRQ path, M2 MPU no-op).
Divergences worth closing for M1, most impactful first:

- [~] **mk64f diag-LED backend ADDED build-only @b5c5665 [DEAD HASH: resolves nowhere -- lost to an earlier history rewrite]** (RED PTB22 active-low) -- code gap
      closed; HW confirm folds into the M2 K64F SYSMPU bring-up (K64F is not an M1 gate, see above).
      **esp32(lx6) DONE** -- GPIO2 (silkscreen D2), validated with `blink` on hardware.
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
  Nothing frees an arena block's ownership record either, so a dead task's blocks stay spent and a
  restart loop drains the arena: an open defect, removed from `arena-block-owner-is-the-reserving-task`
  on 2026-10-21 (invariants audit).
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
  time; the reent seam writes one `struct _reent`, 512 B on arm-none-eabi, 288 on
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
    throw crossing a switch is not covered by either mechanism. Superseded by M10.6.10's
    exception-state entry.
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
    via the already-ISR-safe `sem_post`. New arch surface is small: `arch_cpu_id`, a data memory
    barrier, and a notify/drain doorbell pair (so RP2350 SIO-v2 doorbells back the same
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
      (D) **Comment nit** `regs_v8m.h:28-29`: the PRIVDEFENA-background
          note overstates: a MATCHED region's AP also bounds privileged access.
      **PARTLY:** A to C are in the tree. `pmsav8_encode` (`arch/arm/common/pmsav8_encode.h`) seats only
      an encodable region, the image-window assert in `arch/common/sections.ld.h` holds every Cortex-M
      window to the granule, and `kickos_arm_pmsav8_init` reads `MPU_TYPE.DREGION` and refuses a part
      too narrow. D is open.
- [ ] **RP2350 bench `irq` reads a bogus 1 cyc** (measurement-only): the irq-probe is not wired
      for the M33. Per-chip bench-instrumentation cleanup, not a kernel bug.
- **No gap (already accelerated), for the record:** STM32F411 ART (ICEN|DCEN|PRFTEN + 2WS,
  `chip_stm32f411.cc:171`); STM32F103/F302 prefetch buffer (M3/F3 have no I/D cache in HW);
  K64F FMC cache+speculation on by reset default (`PFB*CR=0x3004001F`); XMC4800 PMU buffers
  default-on + WS set (`chip_xmc4800.cc:373`); RP2040 XIP cache on by bootrom; ESP32-C6 cache
  fronts external flash only, from which task text runs since M10.6.12, mapped and enabled by
  `Reset_Handler`.
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
- [ ] **Missing gates the purge surfaced.** The rule this milestone runs on is that a comment which
      is the only protection for something is a MISSING GATE, so the sweep was asked to report them
      rather than delete them. Every one below is a real constraint held by prose alone. None was
      removed; each needs a test, and none is a comment problem.
      *Kernel*: the `KICKOS_MIN_STACK_SIZE` per-arch floor is never checked against the deepest
      `exit_current` chain, so a wrong override overflows only on thread exit, on hardware.
      `wq_confirm_resume` requires the lock released BEFORE `wait_result` is read; inlining the read
      under the lock compiles clean and passes on the sim, whose switch is synchronous, and races on
      ARM. `ThreadPool` stack harvest must happen only once the exited thread is provably off-CPU.
      `domain_for` requires its posture word resolved by the CALLER, never read from
      `sched::current()` inside. `rearm_locked`'s clear-then-unmask order on a first arm matters only on ARM and RX,
      which are default-masked; sim and riscv would never catch a reorder. `console_tx`'s
      prime-the-pump applies per chip family and a refactor dropping it hangs TX on real
      edge-triggered hardware only.
      *ARM*: `arch_diag_led_init` depends on `uart0_init` having opened the PORTB gate earlier in
      `arch_init`; reordering silently drops the PCR store. `chip_mps2` losing its
      `kickos_arm_pmsav8_init` reference still links and silently writes RASR-shaped values into
      RLAR. `sam3x8e`'s `tc_clock_init` ordering BusFaults a static ctor calling `ktime_now`, and its
      `MOSCXTST` window is a guess pending Due silicon. `imxrt1062`'s MPU-before-cache order
      manifests only as a silicon hang, and that chip has no QEMU model. `MODEM_TXCTSE`
      without real CTS wiring makes the polled writer wait forever. `switch.S`'s FP-frame save order
      is covered by `static_assert` for the scalar offsets only, not for `{s16-s31}` before
      `{r4-r11,lr}`.
      *RISC-V and Xtensa*: the C6 all-ones-NAPOT quirk, why the bootstrap entry uses TOR, is not
      reproducible under QEMU. `arch_pinmux_set` writes the GPIO-matrix out-sel before the
      kernel-owned-pin check, so dropping the matrix stage bypasses console and LED pin protection.
      APM denial does not trap the way PMP does, so a bad region hangs instead of faulting.
      *RX*: setting SCI6 `SCR.TIE` while `SCR.TE` is 1 raises no TXI6 (UM sec.42.12.2), so the ring's
      prime must also write the first byte; a refactor dropping that write stops the console TX drain
      after idle. An earlier
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

- [ ] **The unit of commonality is the DRIVER TYPE, and the taxonomy is layered.** One API per
      peripheral kind, not one universal API and not one per chip:
      **SPI**, **I2C**, **UART**, **USB host**, then **per USB device class** (CDC-ACM first, the
      others as they arrive) layered on the host controller rather than beside it, and equally
      **timer**, **PWM** (open: its own type, or a timer with a capture/compare mode), **one-wire**
      and **GPIO**. That list is examples, not an enumeration: **the rule is general to every driver
      type the fleet grows.** A backend that cannot express its hardware in its type's API is the
      signal that the API is shaped around one vendor -- which is the discovery M4 exists to make,
      so treat a genuine misfit as a finding rather than forcing it.
      **PARTLY:** SPI, I2C and UART each have one class in `user/include/kickos/driver/`; USB
      host, the USB device classes, timer, PWM, one-wire and GPIO have none yet.
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
      **PARTLY:** the compile-time substitution landed: `kickos_select_class_backend` and
      `kickos_link_class_backends` in `cmake/kickos.cmake`, the proxy in `user/lib/spi_proxy/`.
      **RULED (maintainer, 2026-11-05), replacing "per consumer target":** the backend follows the
      BUS, not the consumer. A bus with a service is reached only through that service, by
      everyone. A bus with no service is driven directly by the one image that owns it. Example and
      first test: task A uses SPI0 through its service while task B drives SPI1 itself. Today the
      choice is one global per class per build (`kickos_select_class_backend`), so one image that
      uses a served bus and an owned bus of the same class needs both backends, and their
      `kos_spi_*` symbols collide. That is the design question. Deferred to M11 (the driver era).
      **ACCEPTANCE SCENARIO (maintainer, 2026-11-05):** a KickCAT EtherCAT slave task drives SPI1
      directly: only the ESC is on that bus, and it needs the speed, so a context switch per
      transfer costs too much. SPI0 carries a fan controller, a temperature sensor, an IMU and an
      EEPROM that holds the serial number. Three tasks use SPI0: one drives the fan from the
      temperature sensor, one reads the IMU and pushes it to the slave stack at its own pace, and
      one checks the fan speed for safety. All four tasks, the slave included, read the serial
      number from the EEPROM at startup. The integrator alone is responsible for safety; KickOS
      provides the tool, its ceiling and floor.

      **What KickOS provides** (the tool's guarantees, with its worst cases stated):

      - One image reaches a served bus and an owned bus of the same class: the slave task uses
        SPI1 directly and SPI0 through the service. The backend is chosen per bus at build time
        with no run-time cost: a per-image bus table from the composition, both backends linked
        under internal names, and the public call switching on the bus id, folded away when the
        bus is a constant.
      - Device grants: the service grants a DEVICE (bus, chip select, mode and speed), not the
        bus, with read-only rights where asked. Each task reaches only its own devices.
        The slot-to-chip-select binding comes from board and composition data, and CONFIG stops
        choosing it (today a slot owner can select another slot's device, `bus-service.md`).
      - The unit of work is a transaction: several transfers under one held chip select, with the
        device's mode and speed applied per transaction.
      - No queue in the service: it serves the next caller, and an endpoint already hands it the
        highest-priority waiting caller first.
      - The worst wait is stated: a transfer in progress is never interrupted, so a caller waits
        at most one whole transaction on the bus.
      - Every transfer is bounded (as the `xmcssc` and DSPI bounds are), so a wedged device does
        not block the bus forever.
      - A client request can carry a deadline: zero answers at once, any other value waits up to
        that long, as the task create's timeout does. Today a bus-service client names no
        deadline.
      - A dead service unblocks its callers with `-KOS_EPIPE`, the error a call already answers
        when its server dies; nothing new is needed.
      - Neither the driver nor the service caches device contents: a driver cannot assume what the
        bytes mean (the class must not cook, see that ruling in this section), so the device stays
        the one truth.

      **What the integrator decides** (the scenario shows the tool allows each):

      - The loss of the service is a safety input: if the safety task gets no answer within its
        reaction time, it triggers the safe state, as it would on a wrong fan speed.
      - The safe-state trigger does not travel over SPI0: it has its own path (a fail-safe GPIO, a
        power cut, or a device that falls back to a safe default when the bus goes quiet).
      - Long operations are split into short transactions, so the safety task's worst wait stays
        one short transaction: an EEPROM write's completion is polled as separate requests, never
        held inside one transaction.
      - The EEPROM is read-only to the four readers, and a provisioning task alone holds write
        access to it. After an update only the four readers are stale; nothing else restarts.
        They learn of it either by being restarted (the restart of the EtherCAT slave drops it
        back to INIT, a deliberate choice), or by a "device changed" event from the service, a
        device-event source for the deferred event-source call. The choice is left to the driver
        era.
      - The application may cache the serial number if it wants.
      - The service is supervised for availability.

      Outside the class: the IMU to slave-stack handoff is a ring the slave polls without blocking,
      and the ESC interrupt reaches the slave through the IRQ notification, the first device-event
      source for the deferred event-source call.

## Found reconciling the reference documents with the tree (2026-08-25)

- [ ] **RX owes a `pspguard` witness, and the app as it stands cannot give it one.**
      `user/apps/common/CMakeLists.txt` adds `pspguard` only when `KICKOS_ARCH` is
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
      thread chose, which is the entry `.Lsys_bad_usp` guards. What is missing is a run:
      `tests/integration/gates/faultsurvive.cmake` gives both a bench judge on rxv3 where memory
      is enforced, and no rx72m capture of either is recorded. Without it the other RX arms land
      elsewhere -- `faultsurvive_ovf` overflows the stack and the MPU denies the store, and
      `faultsurvive_off` traps through `mvtipl`, a privileged instruction, so it takes the fault
      path where `kickos_fault_frame_trusted` does the checking. So `kickos_rx_bad_usp` is compiled
      into every RX image carrying fault isolation and reached by nothing that runs.

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

- [x] **NOCACHE and DEVICE are still accepted where no PTE attribute carries them. RAISED AGAIN,
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
      **RULED (maintainer, 2026-11-05), taken by M10.6.12:** rv64 answers NOCACHE and DEVICE per
      physical address. It accepts both on MMIO and refuses NOCACHE on RAM.
      **LANDED:** the chip says whether a page entry carries the type (`arch_aspace_entry_memtype`,
      false on virt_rv64), and `grant_memtype_at` answers per physical address: where the entry
      carries none, NOCACHE and DEVICE only inside a chip-stated aperture, so a self-grant, a task
      block, a frame map or a memory window at NOCACHE over RAM answers `-KOS_ENOTSUP` on rv64.

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
      which the pushed cs no longer describes which base is live. `arch_cpu_id` reads gs,
      so code run in either window can see the wrong base. The exposure is only an NMI or a
      machine check inside the window, both already fatal, so the cost is a lost fault report.
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
      no bench image for the board, and `tools/bench/bench-fleet.sh` does not list it.
      **Standing up a bench variant for this arch is a step of its own** and belongs with M8's
      instrument, where the comparison it feeds lives. Until it is taken, no compact-frame decision
      may be argued -- which is what T7 froze.
      **PARTLY:** the instrument now exists: `boards/qemu-arm64/configs/bench/defconfig` is the
      `qemu-arm64-bench` preset, run by the CI `bench` job. No figure is recorded against the
      compact-frame decision yet.

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
**QW-5. Confirm the cap/handle layer stays address-space-agnostic, and keep it that way.** A
do-no-harm review rule, not a change: the per-task handle to per-kernel object-pool model
(`cap.cc`) is ALREADY MMU-clean, and no cap/endpoint code should start keying on a physical
address or a region base the way `domain_for` does. Object naming stays purely by
handle/slot, never by address. Free, since it is the current design, but endpoint IPC is
exactly the code tempted to stash a shared-buffer physical address in a cap, which would drag
address-space assumptions into the one layer the MMU rewrite relies on being clean.

### Filed out of S5 by ruling, 2026-09-01

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
      **PARTLY:** lx6 and x86_64 now drive several cores and carry a raised set
      (`arch/xtensa/lx6/arch_xtensa.cc`, `arch/x86/x86_64/arch_x86_64.cc`); rv32imac, esp32c6
      and rxv3 still run one kernel core with a single `g_inject_line`.
- [ ] **The cross-core shootdown has no witness on this bench, and that is architecture-mandated
      code rather than a gap to close with a test.** STATE.md already records that removing rv64's
      per-page invalidate leaves every arm green, so the LOCAL invalidate is unwitnessed here; a
      missing REMOTE one is unwitnessed for the same reason and worse, QEMU's TLB model not being
      the machine the ISA describes. What the send has instead is structural: the rendezvous body
      is now reachable and `check_doorbell_generic.sh` asserts it on this arch, where before the
      symbol was dropped for want of a caller. Treated the way the contract treats the data-cache
      seam: correct by construction, recorded as unwitnessed, not described as complete.

- [x] **The RV64 doorbell's INSTRUCTION-side half has no operation at this board's ISA baseline.**
      The service body carries `SFENCE.VMA`, which orders TRANSLATION and which the ISA gives no
      way for one hart to perform for another. The instruction half is `FENCE.I`, and Zifencei is
      absent from `arch/riscv/chip/virt_rv64/cpu.cmake`'s march string, so it does not assemble.
      Ruled DEFERRED rather than papered over: calling `SFENCE.VMA` sufficient would put an untrue
      statement of contract in the tree. No caller exists yet, the rv64 instruction-side rendezvous
      being unwired and dropped by `--gc-sections`, so this is owed by the first caller that needs
      it. Raising the baseline is a real option and not a free one: the toolchain's multilibs are
      named for exact march strings, so a change is measured against them rather than assumed.
      **LANDED:** no half is owed: FENCE.I orders a hart's fetch after stores to instruction
      memory, which no map edit makes, and SFENCE.VMA orders the fetches of later instructions.
      Every RISC-V chip's march carries Zifencei (GCC 16 still picks the same multilibs), and every
      hart runs FENCE.I where it starts on code a loader or another hart stored: _start on both
      QEMU virt chips, and the ESP32-C6 LP stub before it enters node 1's image.

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

- [ ] **The doorbell's interrupt group is the reset value and the secure posture is unexercised.**
      The group register is left as reset with the non-secure attribute clear, matching the
      acknowledge and end-of-interrupt path already in that file. The security-extensions posture
      cannot be exercised on this bench at all: that machine option enters at EL3 and the port's own
      exception-level refusal fires first, from every core at once.
## Gate-surface re-inventory (anytime coherence, not scheduled)

Parked by the user as "at M7" when M7 meant the seam rework; M7 is multicore now, and this is
orthogonal to every milestone rather than gated by one. The user's framing, 2026-08-21: the gate
surface grows exponentially and may not be worth its size.

**A MUTATION HARNESS MUST `touch` EVERY SOURCE IT RESTORES, OR THE CONFIRMING BUILD IS
STALE AND THE GREEN IS THE MUTANT.** The rule for anyone mutation-proving an arm here, and
it applies to `ninja` and to `make` alike. A harness that copies a source aside, patches it,
builds, runs and then moves the copy back hands the build a restored file whose mtime is the
COPY's, which predates the object just built from the mutated source. The build believes the
object is current, the "reverted, green again" run re-executes the MUTANT binary, and the
red reads as a regression in the arm just written rather than as a stale object. Only the
restore direction is poisoned, the mutated builds being newer than their objects, so the
symptom is always a red that appears AFTER a mutation batch and never during one.
`touch` every restored source before the confirming build, or copy with the mtime bumped.

- [ ] **Give every image gate a `QEMU_TIMEOUT` bound of its own, then delete the twelve job-level
      pins in `ci.yml`.** That is the rule the `bench` job already states: a bound belongs in the
      gate, and a job-level value overrides every script's `:=` at once. The pins are still there
      because 68 of the 116 `tests/integration/check_*.sh` set no bound, and for those the pin is
      the only backstop above `tests/lib/gate.sh`'s 20 s in `run_image` and 8 s in `poll_image`.
      All twelve are RAISED to 180 for that reason rather than dropped.
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
| `frdmk64f` + its UART-IRQ service list | 5 | **4** (7 runs total across the day: 5) |
| `xmc4800-relax` + its UART-IRQ service list | 5 | 0 |
| `rx72m` + its UART-IRQ service list | 5 | 0 |
| `esp32-wroom` + its UART-IRQ service list | 1 | 0 |
| `esp32c6-wroom` + its UART-IRQ service list | 1 | 0 |

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

**SUPERSEDED (maintainer, 2026-10-29, M10.6.10 D4):** `exit()` from any thread ends its TASK, as
POSIX says: the task's atexit handlers and stdio flush run in the caller, then the task ends with
the code. `kos_exit` stays the thread's exit. The premise below, a thread as the unit of
isolation, gave way to tasks as processes on every backend in M10.6.9. The text is kept as the
record of the earlier ruling.

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
- [ ] **A fail-closed guard in slay is UNREACHABLE from userspace and its mutant SURVIVES**
      (section 14.5 of the archived M4.8.4 kill-and-slay record): the idle/privileged target refusal, because
      idle is a static TCB outside the `ThreadPool` and root is unprivileged, so no privileged
      thread is resolvable by handle at all. It becomes reachable in the driver era or under M5.
      `kickos_fault_kill_thread` carries the identical rule with the identical reachability
      status, so this is a pre-existing shape rather than a new one.
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
`rr_interleave` fails on `rx72m` under its UART-IRQ service list. Measured rates, one board,
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
the task pool size (one more than the thread count then, so 17 on rx72m) byte compares MASKED, on EVERY
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

- [ ] **The endpoint drain (`answer_senders`) publishes once per sender under one `IrqLock` from a
      LIVE thread**, the same residue as the `sched::wake` supersede but with no dying guard
      involved, and more reachable: fairness only.
- [ ] **`teardown_depth` now has one production consumer and one fixture consumer**, so the
      `cap_teardown_active()` gate in `exit_current` is premise-less. Kept as conservative retry
      throttling; revisit when the DEV-window respawn gate lands.
- [ ] **CDC console throughput is ~167 B/s** under many small writes (2008 bytes in 12 s). A console
      you cannot read a suite through is marginal.

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

- [ ] **`sched::wake`'s guard reads a `kernel().current(me)` that a deferred switch has already moved.**
      `switch_to` publishes `kernel().current(me) = next` BEFORE `arch_switch`, and on ARM, RISC-V and RX
      that only pends. So after one admitted wake inside a sweep the dying thread keeps running with
      `c` naming the PEER, and every later wake in that chunk sees `dying == false` and
      `state == RUNNING`: both new clauses are dead and `reschedule()` runs unconditionally. No wrong
      final state was constructible, because the EPIPE drain pops in DESCENDING priority so
      `pick_next` returns the already-published peer and the extra `reschedule()` early-returns. The
      residue: the guard's stated premise is defeated for later wakes, and two arms in ONE chunk
      waking ASCENDING priorities would run `on_switch_in`/`arm_slice` for a peer that never runs, so
      an RR peer forfeits part of its first quantum. The gate cannot see it (the stub also returns).
- [ ] **The K-seam fixture only ever compiles the SIM posture.** It takes `kickos_kernel`'s
      `COMPILE_DEFINITIONS` verbatim and registers only under `KICKOS_ARCH STREQUAL "sim"`, so a
      SEGMENTED capability table (more than one chunk, which `frdmk64f` runs),
      and every non-sim cap geometry never reach a K-seam arm. That compounds with the chunk-boundary
      arm: flat-versus-segmented teardown IS a chunk-boundary property.
      **PARTLY:** `kickos_add_kseam` takes a POSTURE now, and `kickos_kseam_captable`
      (`tests/unit/captable`) runs the real exit path over a table of several chunks.
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
## Found porting fault isolation to rxv3 (2026-08-12)

- [x] **THE BELOW-STACK TEST (formerly `kickos_fault_below_stack`) NARROWS WHAT A FAULT REPORT
      CAN ATTRIBUTE ON rxv3, and design 4.3's
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
      the dying thread's stack, i.e. section 4.3 of the archived fault-isolation record, which
      already recorded that it "would survive 4.2" and is now measured on an ISA that needs it. It
      would delete the rxv3 test, not tune it. `fault-frame-untrusted-until-proved` called the
      imprecise converse deliberate; that clause left the invariant on 2026-10-21 as this defect.
      **PARTLY:** the rxv3 redirect moves the stub onto the thread's kernel block
      (`kickos_fault_stack_top`), and the below-stack test now counts only an access within one
      16-byte band below the base; the test itself is still there.
      **RULED (maintainer, 2026-11-05), taken by M10.6.12:** the stub now runs on the kernel
      block, so delete the below-stack test and its 16-byte band.
      Witness `mpu_fault` and `faultsurvive_ovf` on rx72m.
      **LANDED:** the predicate and the band are gone, and so is the USP containment test, which
      was what escalated `faultsurvive_ovf` (the recursion's frame drops the USP below the base
      before its first denied store): rxv3 kills every unprivileged fault whose thread has a
      stub stack. `faultsurvive_ovf` and `faultsurvive_off` are judged `killed`, witnessed on rx72m.

- [x] **On a FLAT rxv3 board nothing catches a wild SP that faults with a non-MPU cause.** The
      below-stack test is inside `#if KICKOS_HAVE_MPU` because it reads `MPESTS`/`MPDEA`, and the USP
      containment test only sees an SP that is out of range, not one that is in range with no room
      below. So on `rx72m-flat` an address exception taken by a thread whose stack is exhausted would
      be killed and the stub would run on it. Not witnessed either way -- the flat posture has no arm
      that reaches this -- and it is the same hole 4.3 closes. Filed so the enforcing-only scope of the
      guard is on the record rather than implied by an `#if`.
      **PARTLY:** on every rxv3 posture the redirect resets the USP to the thread's kernel block
      (`kickos_fault_stack_top`, `arch/rx/rxv3/arch_rxv3.cc`), so the stub no longer runs on the
      exhausted stack; still not witnessed.
      **RULED (maintainer, 2026-11-05), taken by M10.6.12:** the stub now runs on the kernel
      block, so delete the below-stack test and its 16-byte band.
      Witness `mpu_fault` and `faultsurvive_ovf` on rx72m.
      **LANDED:** on every rxv3 posture the redirect seats the USP at the thread's kernel block
      unconditionally, so the stub never runs on an exhausted or wild USP, flat included.

## Found landing task-layer step 9.3 (2026-08-11)

- [ ] **`microbit` has ZERO arena slack, so no `.bss` addition is ever inert there again.**
      `__kickos_ram_start` IS `_ebss` and the granule is 32 bytes, so four bytes cost as much as
      thirty-two. Step 9.3's task pool took `mem_self_grant`'s last `kos_ram_alloc` grain and that arm
      is now a declared skip. The structural options, none taken: shrink the board's static demand,
      give the selftest a microbit-specific arena reservation, or
      accept that this board's skip set grows once per milestone. Worth deciding deliberately rather
      than one arm at a time, because the next `.bss` byte from ANY change takes the next arm.

## Found landing task-layer steps 9.4 and 9.5 (2026-08-12)

- [ ] **Nothing witnesses the DEATH POINT on silicon.** `tests/unit/taskdeath` gates that a peer is
      marked and made runnable, `sim_driver_death` and the `task_group_kill` selftest arm gate that it
      then dies -- all under a host or emulated clock. The interesting case is a driver cancelled while
      its IRQ line is armed on real hardware, where the wake races the device. Wants an enforcing board
      with an IRQ UART service list, which is what makes it a fleet-pass item and not a gate.

## Found during M6.3 and M6.4, deferred with the reason, then fixed at the pre-audit step (2026-08-28)

- [ ] **OWED MOVE: the three byte-seam DEFINITIONS are in the syscall layer while their declarations
      are in the memory layer.** `kaccess_from_user`, `kaccess_to_user` and `ep_copy` are declared in
      `kernel/include/kickos/aspace.h` (lines 41-52), which is where R6 moved them from
      `kernel/syscall/syscall_internal.h`, but they are still defined in
      `kernel/syscall/syscall_mem.cc` (lines 691-772) over a file-local `access_copy` (line 563).
      They arguably belong in `kernel/mem/aspace.cc` beside the rest of the address-space code, and
      the move did not happen only because another agent owned that file during the pass. No
      behaviour depends on it; it is a layering tidy-up with one caller-visible consequence, that a
      reader following the header lands in the syscall directory.

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

## The libc lock and the mutex fast path (maintainer, 2026-10-11)

- [ ] **Three comments in the red-zone indirect-call records cite evidence that no longer holds.**
      One cites three lines of `console_tx.cc` and one a line of `chip_esp32.cc` for the
      ConsoleTxBackend, and those lines now hold other code. The comment above the lx6 NONE block
      says `task_object_admit_grants`' one `jx` is its switch's jump table, but no reclaimwit image
      (esp32-wroom, -smp, -bench) shows a `jx` in that body; its record still resolves (found
      2026-10-21).
      **PARTLY:** the console_tx.cc and `jx` comments are gone; the chip_esp32.cc:306
      citation in `tests/static/trap_redzone_indirect.txt` is still stale, the backend now
      living in the espuart driver.

- [ ] **PERSISTENT STORAGE AND FILES: KEEP THE DOOR OPEN (maintainer, 2026-10-21).** Not numbered
      yet, and it will come: an OS with no writable persistent storage is soon useless (an EEPROM or
      flash over SPI, an SD card over uSDHC, a file system on top). Today newlib's file stubs ignore
      the descriptor (every write goes to standard output, a read is end-of-file, `_open`
      refuses every path). The shape that fits the microkernel: storage drivers and file systems are servers;
      each task's runtime keeps a descriptor table in user space, an entry being a capability on a
      server's endpoint plus the server's handle for the open file, guarded by the libc lock;
      `open` reaches the file system by path; `read` and `write` are IPC calls, a large transfer
      going through a shared memory grant; descriptors 0, 1 and 2 are ordinary entries on the
      console. The console will take input too (stdin, for a shell, planned by the maintainer),
      so descriptor 0 reads from the console server like any other file. The kernel question it
      raises: handing a capability to a client inside an IPC reply (at `open`), with its rights,
      revocation and what a task's death does to it. No design may close this door.

- [ ] **MEASURE FIRST: A THREAD THAT NEVER USES THE FPU SAVES NO FPU STATE (maintainer, 2026-10-17).**
      armv8a and x86_64 save and restore the whole FPU state at every switch (an 832-byte XSAVE
      area on x86_64). The candidate: every thread starts with the FPU disabled
      (`CPACR_EL1.FPEN`, `CR0.TS`), its first FPU instruction traps, and the kernel marks the
      thread as an FPU user, enables the FPU for it and re-runs the instruction; from then on its
      state is saved and restored eagerly at every switch. NOT lazy switching, which leaves the
      previous thread's registers live until the next thread touches them: that is the LazyFP
      leak (CVE-2018-3665) and is refused. ARMv7-M already gets this from its lazy stacking, and
      RISC-V can save only a dirty state by `mstatus.FS` with no trap. Measure first: the save's
      cost per switch, and how many threads of real workloads touch the FPU at all (a compiler may
      use SIMD registers for plain copies, which would make the trap fire almost always). It would
      be the first fault a task survives.

- [ ] **MEASURE FIRST: A YIELD WITH NOBODY TO YIELD TO TAKES NO KERNEL LOCK (maintainer, 2026-10-13).**
      A thread storming `yield` takes the one kernel lock on every call, even when no other thread
      at its priority is ready on its core. The FIFO ticket lock already bounds what that costs
      other cores (at most one extra critical section per acquisition, which the worst case
      assumes anyway), so this is throughput, not latency. Measure first: on `qemu-arm64-smp`, how
      much one yield-storming core delays another core's syscall round trip (the SMP bench). If it
      shows, a lock-free check returns at once when no peer is ready; a wake racing the check is
      corrected by the waker's own reschedule, at worst one yield skipped. A host case pins that
      race. A storm on other cheap syscalls stays a cost of the one-lock design.

- [ ] **THE CONTENDED SEMAPHORE IS SLOWER THAN THE KERNEL SEMAPHORE WAS (accepted for now,
      maintainer, 2026-10-22).** "A always contended sem is either a sync barrier or a bad design
      so it is OK", and "it does not mean that it is a final state and any optimisation that can be
      done is welcomed": fewer kernel objects and a far faster uncontended path are the better
      shape, and anything that cuts the contended cost is wanted. Where the time goes (M10.6.8,
      profiled under QEMU): a contended wait or post names the word by its memory, a write check
      and a page walk, then finds, takes and frees a wait record; on arm64 QEMU each kernel entry
      also empties the emulator's TLB. Measure before and after on the bench's switch rate and
      `sem-handoff`, on QEMU and silicon.

- [ ] **`qemu_arm64_stress`'s 20 s bound is marginal on a loaded host.** At load 30 to 40 it ran
      2 to 20 s on the closing tree and up to 16 s on its base, and timed out twice in a parallel
      suite; alone it takes about 1.5 s. Size it as a hang detector, as `libcrace`'s was.
      **PARTLY:** above one core the stress gate's bound is 1400 s
      (`tests/integration/gates/stress.cmake`); a one-core preset keeps the 20 s default.

## Found closing M10.6.4 (2026-10-09)

- [ ] **M10.8: `amp_hold` fails when the whole binary runs in order.** A suite that passes alone
      fails after its neighbours in the one binary, so the order of the cases matters.
- [ ] **M10.8: The self-grant map takes time under `IrqLock` that grows with the run size.** The
      map walk for a long run holds interrupts masked for its whole length.
- [ ] **M10.8: `qemu_riscv64_stress` runs 60 to 100 times slower under host load**, on the base
      tree and on this one alike, so a loaded box turns its bound into a failure.
      **PARTLY:** above one core the stress gate's bound is 1400 s
      (`tests/integration/gates/stress.cmake`); a one-core preset keeps the 20 s default.
- [ ] **M10.8: The `qemu-arm64` expected faults `was` and `wro` never fault.** The gate accepts
      the absence.
- [ ] **M10.8: A far call answers empty where a local call waits.** The two paths of one call
      differ, an asymmetry of the ABI.
- [ ] **M10.8: `errnoprobe` arm F no longer guards a switch-in.** The guard it exercised is gone.
- [ ] **M10.8: Whether the selftest's converted worker cells needed atomics is unanswered.** The
      cells main reads only after joining their worker say so and stay plain; the others were made
      atomic in one sweep without asking whether a join already orders them.
- [ ] **M10.8: `confused_deputy`'s partial still says the arena is too small for the grandchild's
      stack** where the refusal is a thread slot.
## M10.6.9: MPU tasks as processes and the kernel isolation holes (maintainer, 2026-10-21)

- [ ] **M10.6.9: CHECK WHETHER THE ESP32 CAN SPLIT USER FROM KERNEL (found 2026-10-21).** The LX6
      port has no user/kernel split: `PS.UM` is 1 for kernel and thread alike, so a fault there
      panics instead of killing its task. No record examines the ESP32's PID controller and its
      MMU and MPU permission scheme. Establish from the ESP32 TRM whether they permit a user/kernel
      split. If they do, the LX6 port owes the split. If they do not, it is the hardware floor
      (maintainer, 2026-10-21) and `docs/reference/boards.md` states it.
      Cortex-M0 without the privilege extension is a real floor.
      **PARTLY:** the check is done: `docs/reference/boards.md` ("The ESP32 runs KickOS flat") says
      the PID controller could isolate tasks, gives its cost, and keeps the ESP32 flat.
      **RULED (maintainer, 2026-11-05), taken by M10.6.12:** the intent is to protect the kernel
      from user code, not to isolate tasks from each other: every task runs on one unprivileged
      PID. A design note comes first.

- [ ] **NO PACKAGED DRIVER'S STACK IS BOUNDED BY THE APP-STACK GATE (found in M10.6.9 step 8b, 2026-10-27).**
      `tests/static/app_stack_roots.txt` held a record for one app thread only; no packaged driver's
      threads (service and IRQ workers, rooted at `kickos_driver_task`) are bounded against the stack
      they run on. Step 8b gave the gate an `above=` key and bounded the app-local `appsink` from its
      real entry; do the same for every packaged driver, against the stack its composition or
      descriptor states.
- [ ] **THE `appdata_no_kernel` GATE CANNOT COMPARE HIGH-HALF ADDRESSES (found in M10.6.9 step 8a5,
      2026-10-27).** It parses addresses with awk doubles, which lose the low bits of a 64-bit kernel
      high-half address, so a kernel record planted at the start of such a window goes unseen. The
      MMU program images therefore got no window check for their kernel-placed section, which takes
      only the image objects by exact name. Compare addresses exactly, then give that section its
      window.

## M10.6.10: C++ and libc conformance (maintainer, 2026-10-21)

Every toolchain family is configured `--disable-threads` (`conan/toolchain/conanfile.py`), so GCC
reports "Thread model: single" on every target, and libstdc++, libsupc++ and libgcc compile their
thread safety out while KickOS runs threads, SMP included. That breaks "Applications can use full
C++" in the Book and "Userspace: full C++" in `docs/reference/architecture.md`. The fix is a
toolchain release giving GCC a KickOS threads model. Found 2026-10-21 from the shipped 1.1.0-1
archives (objdump, nm and readelf), nothing built.

- [ ] **`getentropy` HAS NO SOURCE ON ANY BOARD (ruling D7 (a), 2026-10-29).** The chips with a true
      random generator: nRF51 (RNG), RP2350 (TRNG), ESP32 and ESP32-C6 (RNG), K64F (RNGA), RT1062
      (TRNG), and RDSEED on x86_64. Until then `arc4random` raises SIGKILL on its first draw.
      **X86_64 LANDED (maintainer, 2026-11-03: real entropy where the hardware has it, never
      invented):** RDSEED where CPUID reports it, drawn again until ready; EINVAL for a request
      over GETENTROPY_MAX (256); ENOSYS without it. The emulated processor offers it.

## Found by the M10.6.10 review (2026-11-02)

Defects the ten-angle review of M10.6.10 found that the milestone did not introduce.

- [x] **A WORD BROADCAST IS QUADRATIC WITH INTERRUPTS MASKED.** `wake_popped` in
      `kernel/sync/sync.cc` pops each waiter with `wq_pop_highest`, which scans the whole queue,
      all under the waker's `IrqLock`. A broadcast to n waiters of one condition variable is n
      scans of up to n waiters, so its masked time grows with the square of the waiter count,
      bounded only by the thread pool. The C++ condition variable's `notify_all` is a new caller.
      No test pins the broadcast's worst case.
      **LANDED:** a wake of every waiter sorts them once, one stable pass per priority bit that
      differs among them, and wakes them in the same order as before, so its masked time is
      linear; a wake of fewer than all still scans once per waiter, so the waiters it leaves keep
      their queue order. The bench's `word-broadcast` row measures it at three steps.

- [ ] **`task_cancel_group`'S MASKED WALK HAS NO IRQ-OFF MEASUREMENT.** In `kernel/thread/park.cc` it
      walks every thread slot under the caller's `IrqLock` and slays each member of the task.
      `exit()`, `abort()` and an uncaught exception now reach it from any thread of a task. No
      interrupts-off figure exists for the largest pool on the slowest board: owed a bench
      measurement, not a code change.

- [ ] **A TOOLCHAIN WITHOUT THE BOARD'S MULTILIB PASSES THE FULL NEWLIB CHECK.**
      `kickos_require_toolchain_newlib` (`cmake/cross_newlib.cmake`) finds the full profile's
      `libc.a` with `-print-file-name`, which falls back to the default multilib's directory. A
      toolchain built without the board's multilib (riscv without rv32, for one) passes the
      refusal that names a missing multilib, and the failure comes at the first link, where ld
      skips the incompatible archive. Look the archive up with `_kickos_multilib_file`, as the
      nano branch does.

- [x] **THE FULL NEWLIB PROFILE LEAKS A DEAD THREAD'S DTOA LISTS.** `user/src/newlib_reent.cc`
      releases a thread's libc scratch at its exit only under `_REENT_SMALL`, the nano profile. In
      the full profile `struct _reent` keeps `_result`, `_p5s`, `_freelist` (a calloc'd array and
      its Bigints) and `_cvtbuf` as heap pointers, which the next thread in the slot loses when
      its state is primed from the template. A full-profile image whose workers each format a
      double or call `strtod` and then exit, again and again, shrinks its heap until `malloc`
      fails (x86_64, aarch64, or a full image beside a nano one). `errnoprobe` skips `strtod` on
      the full profile, so no arm sees it.
      **LANDED:** a thread ending through `kos_exit` frees its dtoa lists, conversion buffer and
      signal table under either profile, and `errnoprobe` arm E converts and formats a double and
      sets a handler on both.

- [x] **`_close` FAILS WITHOUT SETTING `errno`.** `user/src/newlib_stubs.cc` returns -1 and leaves
      errno as it was, so `fclose(stdout)`, `close(1)` and a failed `std::ofstream` close report
      a stale errno. POSIX names EBADF.
      **LANDED:** closing a console fd closes it for the rest of the task, every call on it then
      answering EBADF, and closing any other fd answers EBADF.
      **RULED (maintainer, 2026-11-05):** that close stands: it is the POSIX behaviour.

- [x] **THE CONSOLE LOOKS LIKE A SEEKABLE TERMINAL FILE.** In `user/src/newlib_stubs.cc` `_lseek`
      answers 0 to every seek and `_isatty` 1 for every descriptor. `fseek(stdout, 100, SEEK_SET)`
      succeeds while `ftell` stays 0, so the usual fseek and ftell size probe takes the console
      for an empty file. A terminal answers a seek with ESPIPE.
      **LANDED:** only fds 0 to 2 are open, a terminal each that answers a seek with ESPIPE; fd 0
      reads end of file and fds 1 and 2 write, and every other fd answers EBADF.

## Found during M10.6.11, older than it (2026-11-07)

Defects and gaps M10.6.11's work met that the milestone did not introduce.

- [ ] **THE `ninja x6_image` PROBE DOES NOT LINK.** `arch/x86/x86_64/nokernel_x86_64.cc` defines no
      `kpanic_enter`, `kputs` or `kickos_terminal_outcome_apply`, which the probe image's objects
      call (`cmake/x86_64_boot.cmake`). Broken on master too.

- [ ] **AN x86_64 IMAGE 0 WITH NO BSS IS REFUSED, AND MAY BE MISPLACED, TWICE OVER.** Both are
      theoretical: libc always brings a bss.
      - `pe_fragment` in `tools/image-link.py` refuses a program with no bss whose data does not
        end on a page.
      - `tools/x86_64-map.awk` may drop the store's input line when the pre-link's
        `.kickos_image0_bss` output section is empty, answering a false "image 1 has no place in
        .kimgs". Unverified.

- [ ] **THE BENCH REFUSED ONE TEENSY CAPTURE ON A ONE-SECOND STAMP MISMATCH.** `usbcdcwit`'s app
      stamp read 14:12:00 against the flashed image's 14:12:01 (tag m10611u15); the rerun passed.
      It points to a bench-tool comparison of two clock reads taken across a second boundary. Not
      chased.

- [ ] **`st_ram_seat` ASSUMES AN ADDRESS ORDER WHERE A TASK HAS ITS OWN ADDRESS SPACE.** Under
      `KICKOS_HAVE_ASPACE` it seats one granule, reserves every arm's block, then seats a second,
      and requires every block to lie between the two (`blocks_between` in
      `user/apps/common/selftest/selftest_common.cc`): it assumes the virtual allocator hands out
      addresses in rising order. An allocator that does not fails the image with a bail-out, not
      silently. A seat that does not rest on the order, or a ruling.

- **A gate's scratch directory may have been deleted under it (no fix owed).** Before M10.6.11,
  `tests/lib/gate.sh` could remove a gate's scratch directory from a subshell wherever a gate
  called `poll_image`, or a uefi-pe `run_image`, in one. M10.6.11 fixed it; no audit was made of
  which gates it reached.

## Found during M10.6.12, older than it (2026-11-12)

- [ ] **M10.8: `sem_units_across_cores` CAN TIME NOTHING.** Its clock starts after main has
      created the takers and posters (`user/apps/common/selftest/selftest_smp.cc`), so a run where
      they finish before main resumes prints 0 ms: one `qemu-arm64` SMP run did, where others print
      2 to 20 s. Only the printed figure is vacuous; the counts it checks still hold.

## Found by the invariants audit (2026-10-21)

Each stood in `docs/reference/invariants.md` as a limit with no maintainer ruling. An invariant
states a guarantee, so the clause left it on 2026-10-21 and the defect is recorded here.

- [ ] **OPEN QUESTION FOR THE MAINTAINER: SHOULD `AUTH_IRQ` NAME ITS LINES? (found 2026-10-21).**
      Minting a `CAP_IRQ` needs `AUTH_IRQ`, and nothing names which lines, so a task holding it
      claims any line that no driver holds and the kernel does not drive. The composition already
      names each task's lines, so a per-line authority could come from it. The maintainer
      (2026-10-21): possibly fine as it is, and not an invariant either way. Not a defect.

- [ ] **A SEAM STORE TO A CLOCK-GATED BLOCK CAN END THE SYSTEM.** The privileged write seam requires a
      tabled block to be clocked and no backend enforces it: a store to a gated block faults
      inside the seam and terminates the system. Latent: USIC0 is ungated by
      `kickos_xmc_usic_init()`, and a gated U1C0 or U2C0 entry would expose it. The seam checks
      the gate, or the allowlist admits only blocks the kernel keeps clocked.

- [ ] **A MISPLACED MUTEX WORD WORKS UNTIL ITS FIRST CONTENTION.** A word the user compare-and-swap
      can operate but the kernel refuses (one in the AMP share, for one) takes and releases
      uncontended and answers -KOS_EINVAL only when a second thread contends. The 2026-10-16
      ruling refuses such words; the late refusal is unruled.

- [ ] **ON THE SIM A RAW MUTEX CALL ON AN UNMAPPED WORD KILLS THE HOST PROCESS.** The sim admits any
      aligned address outside its arena as a word and the kernel reads and writes it as given, so
      a raw system call naming an unmapped or read-only one faults the host process. Every other
      pointer syscall on the sim refuses host-heap memory.

- [x] **A FAILED HANDLE WRITE LEAVES AN UNNAMED CAPABILITY INSTALLED.** `cap_out_deliver` writes the
      handle after the object exists and the capability is installed, so an EFAULT there leaves
      the caller holding a capability at an index it was never told, reclaimed only when its
      holder dies. An agent rejected undoing it, since a close cannot retract a thread already
      running; no ruling. Install after the handle write, or undo per type, as the reply cap, the
      frame-run and the own-space mints already do.
      **LANDED:** every capability mint closes the capability again when its handle cannot be
      written, so a refused delivery leaves the table and the object as they were. A thread or
      task create writes its handle before the new thread can run, and a notification wait or a
      fused receive takes its bits only once they are written back.

- [ ] **A SUPERVISOR CALLER TRAPPING WITH THE MASK RAISED IS NEITHER CLEARED NOR REFUSED.** The ARM
      and RX syscall slow paths do neither; the ARM IPC fastpath refuses on `ICSR.PENDSVSET`.
      Latent: no such caller exists in the tree. Refuse it on the slow paths as the fastpath
      does.

- [ ] **THE PMSAV7 RECORD IS PER CORE BUT ITS SLOT BASE IS CHIP-WIDE.** `mpu_held_invalidate()` clears
      only `arch_cpu_id()`'s record, while `g_fixed_count`, the first per-thread slot index, is
      chip-wide, and the stash `g_pend_image` is one global. On a multicore PMSAv7 part where each
      core ran the fixed-region init, a core's skip could compare the right words against the
      wrong slots. Latent: the only chip declaring fixed rows is single-core. A per-core form of
      both, or a build refusal of fixed rows on a multicore chip.

- [x] **THE LOCK-FREE AUTHORITY READ WAS NEVER REVISITED FOR SMP.** The authority check reads the
      caller's own word with no IrqLock. The invariant said an SMP kernel must revisit that with
      the rest of the IrqLock contract, and `docs/design-multicore.md` does not. Confirm the read
      is safe above one core.
      **LANDED:** safe: once a thread runs, only its narrow writes its word and only clears bits,
      so an unlocked read never shows a bit it did not hold; `cap.h` states that rule.

- [ ] **A `KOS_DRV_EP_RETAIN` SEAT IS OUTSIDE THE BUILD'S ENDPOINT COUNT.** A driver an app brings up
      under it keeps a further endpoint slot in root that no build can read, so root can boot
      past its own ceiling on a board that configured and built cleanly.

- [ ] **A CLOCK RETUNE IS REFUSED WHILE A DRIVER OWNS THE CONSOLE.** The retune returns the unchanged
      rate unless the console is kernel-owned, since the kernel cannot re-derive a userspace
      driver's baud. A retune that tells the console driver, or a ruling.

- [ ] **A STORE INTO FRAMES WHOSE MAPPING THEIR OWNER IS CHANGING MAY BE LOST.** One the kernel
      delivers into them for an IPC sender included; the loss stays inside the owning task's view
      of its own frames. And bytes that share a cache line with a kernel copy are read back and
      written back with it, so a task writing them from another core during the copy races the
      kernel as it races a device on a line it shares with a DMA buffer.

- [ ] **THE SCHEDULER EXCLUSION TOKEN IS COPYABLE AND STORABLE.** A `Held` shows its holder was handed
      a lock, not that the lock is held now (`kernel/include/kickos/held.h`). A token that cannot
      outlive its lock scope, or a ruling.

- [ ] **A ROUND-ROBIN QUANTUM IS WALL-CLOCK, AND AN AGENT REFUSED CPU-TIME ACCOUNTING.** A thread
      preempted inside its quantum loses that part of its slice: no equal-priority peer waits
      longer than one quantum of real time, but no thread is promised its quantum of CPU. The
      refusal (it needs a switch-out hook and a remaining-slice field, and weakens the peer bound
      to "eventually") has no maintainer ruling; the 2026-10-13 ruling covers only the floor. Put
      it to the maintainer.

- [ ] **FAULT AND CONSOLE DIAGNOSTICS ARE LOST IN FIVE CASES.**
      - In a double-fault race the first thread's PC is lost: the record prints `PC lost to a later
        fault`.
      - A fault record is lost when the held store has no room, on a console with no ring (an
        own-image AMP node, the chips with no buffered backend), and when the system ends before
        the console driver writes it.
      - A console driver's own prints go to the kernel console, which drops them while that task
        owns the UART.
      - An ordinary line the console ring cannot take does not go out.
      - A synchronous fault handler reading the clock between the three anchor stores of a retune
        reads a torn anchor.

- [ ] **A JOINER CANNOT READ A THREAD'S EXIT CODE.** `sched::exit_current`'s waiter sweep sets the join
      wake result to 0 for every exit, clean or faulted; only a task's creator reads a status,
      through `kos_task_state`. A join that delivers the code, or a ruling.

- [ ] **TELEMETRY CANNOT BE ENABLED ON THE RV64 VIRT BOARD.** `CHIP_VIRT_RV64` selects no
      `HAS_TRACE_CLOCK` (`arch/Kconfig`).

- [ ] **ON THE MULTI-INSTANCE SIM A STATIC MUTEX WORD CONTENDED ACROSS INSTANCES RESOLVES A FOREIGN ID.**
      App statics and the host heap are shared across the sim's kernels, so a word contended
      across instances resolves another kernel's thread id against the local pool. Low.

- [ ] **A DEAD PUBLISHED CONSOLE STAYS CHARGED TO EVERY HOLDER'S TASK.** The stdout seat names the
      published endpoint, a dead one included, until the next publish, and each holder's task
      pays for it until then. Low.

- [x] **A SECOND AUTHORITY NARROW ANSWERS -KOS_EBADF.** A narrow to 0 succeeds and leaves the word 0,
      so a second one, which gives up nothing, is refused (`kernel/syscall/cap.cc`), where
      narrowing an empty set elsewhere does nothing. Make it a no-op, or a ruling.
      **RULED (maintainer, 2026-11-05):** the -KOS_EBADF stays, as the ABI documents it.

- [ ] **PROPERTIES NO RUN WITNESSES.**
      - On armv8a, that the reserved ASID keeps a stale user entry from hitting: QEMU's TLB is not
        ASID-tagged. On x86_64, a real NMI or machine check at ring 3.
      - The cache maintenance the region rule does on the Teensy 4.1, the one board whose cache it
        acts on: it has no emulator, so the host gates and the build stand in. Its capture rides
        M10.6.11's bench pass. No emulator models a data cache, so on every emulated board the
        witness of the alias maintenance is the count of its calls, and the data arm
        `uncached_alias_sync` can fail only on a real cache.
      - The descriptor-budget refusal of the three region backends: no part in the fleet is near
        either limit.
      - The sim's `grant_reserved` 4 KiB case is vacuous: encodability already refuses it.
      - A mutex word whose dead holder's id aliases a live thread after 2^24 to 2^29 reuses of one
        slot (`docs/reference/architecture.md`, "What the mutex word reports, and to whom").
      - A space's data copied while a sibling updates the heap: no arm can time the interleaving,
        and the pristine source is what makes it irrelevant.
      - A `KICKOS_TELEMETRY` gate around code that is not record emission: no gate compares a
        telemetry-ON build's behaviour with a telemetry-OFF one, and every emulated tree runs
        telemetry OFF.
