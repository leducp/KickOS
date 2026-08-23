<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

**M5 IS DONE AND MERGED: ten PRs, `M5.1.1` through `M5.1.10`, master at `a41856d6`.** The driver
era plus everything for SMP that is not SMP. `M5` the integration branch is fully carved and can be
deleted, along with `M5.1.1`, `M5.1.4-docrewrite`, `hotfix/wake-parks-wrong-thread` and every
`topic/*`. Master is still `a41856d6`.

**M5.2.1: EVERY PER-ARCH CONVERSION IS DOWN, AND SO IS PR 7. TWENTY-THREE COMMITS ON THE
BRANCH, NONE PUSHED.** The tip hash is deliberately NOT written here: it names the commit that
writes it, so any amend falsifies it immediately, which happened once. `git rev-parse --short HEAD`
and `git rev-list --count master..HEAD` are the authorities, and this line has been wrong four
times for exactly that reason.** The death path now runs on the dying thread's own
kernel block on ALL FOUR backends, measured as `EXITK` against the block, with `RET`
(`kickos_thread_return`, which nothing relocates) measured against the spawn floor and an `EXIT`
fallback on the four armv7m presets that carve no block.** Derive the count, never copy it: this line
has said twelve, thirteen and fifteen while `git rev-list --count master..HEAD` said otherwise, the
disagreement coming from whether the two pre-rework commits and the session records are counted.
`202ba8cc` PR 3's owed proofs, `ab9a9866` PR 4 rv32imac, `9b382b18` PR 5 both ARM backends,
`7943e6fb` PR 6 rxv3, `3699644b` the PR 4 audit corrections, `26361ecf` the armv7m preset coverage,
`72f91136` the RX privilege boundary, `57fe26c1` the re-taken witness, `ded8a659` the audit's
surviving findings, `778918f1` PR 7's EXIT classes. Read
`~/.claude/projects/-home-leduc-projets-KickOS/docs/m5.2.1-trusted-context-plan.md` section
"SESSION 3" before planning PR 7: it carries what each arch's transfer actually looks like, the
figures that moved, and the debts.

**NO PRIVILEGED C DISPATCH RUNS ON A POINTER A THREAD CHOSE, ON ANY ARCH.** That is the
milestone's central claim and it now holds. What each arch kept is NOT the same thing and the
difference is load-bearing:

  - **rv32imac** transfers on the U-mode accept path. A PRIVILEGED thread's ecall does NOT
    convert: it arrives with `mstatus.MPP=M`, so `.Ltrap_from_m_ctx` keeps frame and dispatch on
    its own stack, which is what the `SYSPRIV` class and the C floor assert exist for.
  - **ARM** converts both privileges together, `.Lsvc_slow` clearing `CONTROL.nPRIV`
    unconditionally, so there is no `SYSPRIV` analogue. The transfer lives in `svc_trampoline`
    and NOT in `.Lsvc_slow`: fabricating a frame there loses `CONTROL.FPCA`, and a PendSV taken
    with it clear does not save `{s16-s31}`, silently corrupting FP state across a blocking
    syscall. In the trampoline the hardware computes the resume PSP.
  - **rxv3** converts both arms, generic and IPC fastpath. Its 236-byte save STAYS on the USP
    because `rte` pops PC then PSW from R0 and R0 IS the USP once the popped PSW sets U, so a
    frame on the block would resume a user thread with its SP in kernel `.bss`.
  - **f302nucleo and due KEEP THEIR RED ZONE**, so armv7m carries BOTH paths under
    `KICKOS_KERNEL_STACKS`. Gated on the chip capability `HAS_MPU` and not the posture, because
    affording the blocks is a RAM question and RAM does not change between a board's enforcing
    and flat variants. sam3x8e is classified by a proxy: it HAS an MPU and lacks only the
    backend, and its scraped granule of 32 CONTRADICTS its own `HAS_MPU`.

**THE PATTERN THAT COST THE MOST TIME, AND IT APPEARED FOUR TIMES: A FIGURE CHARGED TWICE.** The
armv7m FP term charged the same pessimism on both sides and refused a legal thread. The bench
`SYSPRIV` would have made every non-bench board reserve for brackets it never compiles. The
armv7m `SVC` figure would have charged f302nucleo and due for postures they never build. And
the `.appdata` window on rx72m is a cliff rather than a slope. Suspect it whenever one figure
covers two postures, and prefer a posture-dependent macro to a bigger number.

**THE SECOND PATTERN, ALSO FOUR TIMES: A PRESET NOBODY MEASURED.** f302nucleo-st and
bluepill-c8-st were never measured while the class they depend on was live; after three boards
converted, no REGISTERED armv7m preset measured that class at all; the two rv32imac bench
variants fell through the ladder in silence; and xmc4800-relax-st was OVERWRITING ITS CANARY by
four bytes on hardware. **That class is now closed structurally**: the ctest ladder no longer
holds a list, it derives the preset name and asks `trap_redzone_roots.txt`, so the two cannot
disagree. Every preset of every gated arch is declared, and the roots-file header carries the
measured reason each knob is relevant. 22 of the 34 armv7m presets had no console bindings, so
their figures had been LOWER BOUNDS.

**READ THE COUNT, NEVER THE PERCENTAGE, AND SOMETIMES NOT EVEN THE COUNT.** microbit's ctest
count is unchanged at 36 while THREE of its TAP arms lost their real run to the arena
(`endpoint_crossdomain`, `region_mode`, `mem_self_grant` went SKIP, 17 skips to 20). Only a diff
of the TAP stream shows it. It is paid because microbit is the ONLY armv6m board that executes
anything: it runs 12 emulator arms where picopi-st runs none, RP2040 having no QEMU machine.

**THE FLEET AT `72f91136`:** sim 294, sim-telem 296, qemu 53, qemu-m3 50, qemu-m33 52,
qemu-telem 48, qemu-riscv 45, microbit 37, frdmk64f-st 22, rx72m-st 21, esp32c6-wroom-st 22,
xmc4800-relax-st 22, picopi-st 21. Every one gained the `trap_redzone_decls` arm, and
`frdmk64f-st`, `esp32c6-wroom-st` and `xmc4800-relax-st` each gained the `trap_redzone` arm they
lacked against the `6c200e56` baseline, which is the comparison "the three that moved" used to name
without stating. Image sweep 52 presets, 373 gates, 0 fail, carried forward from the `26361ecf`
paragraph rather than re-derived at this tree. Nine more boards build.

**THE HOST HALF IS PAID AT `ded8a659`: 52 presets, 52 pass, 0 reused, 0 fail.** `0 reused` is
load-bearing, `SWEEP_FORCE=1` having made every preset genuinely re-run. That is the half this file
correctly says had never been run once.

**AND THE DEFAULT SWEEP OUTPUT DIRECTORY IS A TRAP.** `/var/tmp/kickos-imagesweep`, which is
`sweep_image_gates.sh`'s default `SWEEP_OUT`, still holds a 09:34Z run from BEFORE `ab9a9866`
reporting **374 gates, 1 fail** (`qemu_riscv_trapnest` on `qemu-riscv-flat`). Anyone re-reading the
default path today reads a pre-PR-4 verdict as if it were current. Name a fresh `SWEEP_OUT` per
tree, and read the `finished` timestamp before believing any summary in there.

**THE rxv3 SILICON WITNESS IS TAKEN, and it is the only execution evidence that arch can
have: RX has no QEMU machine.** rx72m on GlaDOS, tree `72f91136` CLEAN, one boot, validated by
hand through `check_tap_stream.sh` because `bench.sh` does not: `1..105`, 105 ok, 0 skip, 0
partial, `mpu enforce`, banner `kstack 1104 B x 17 = 18768 B`. An earlier pass at `26361ecf`
is SUPERSEDED: an audit found a critical in the code it exercised, so it witnessed a tree
that no longer exists. The arms that make it a witness
rather than a smoke test: `ok 43 call_reg_fastpath`, whose over-budget form is a DECLINE and so
the ONE silent failure mode the fastpath conversion introduced (a bad R0 reload would dispatch
with corrupted args, no fault and no panic); and `ok 19 mutex_chain_boost` plus `ok 47
call_server_death`, which block mid-dispatch and so exercise PendSW's new either-stack leg,
whose failure would panic every blocking syscall. The PRE-EXISTING rx72m witness is superseded
and must not be cited for this tree.

**AND IT PROVES THE ACCEPTANCE PATH ONLY, WHICH IS THE ASYMMETRY THAT LET A CRITICAL LIVE.**
Every blocking syscall drives the block leg, so a 105-arm run exercises the accept side
continuously. NOTHING on RX can reach the REFUSE side: `pspguard` is armv7m/armv6m only with no
RX analogue, and `.Lsvc_nokstack` is structurally unreachable, every pool thread getting a
block. So a green run says the guard does not reject what it must accept, and says nothing about
what it must reject. **An RX `pspguard` is OWED**, and until it exists no rxv3 refusal figure is
witnessed by anything.

**THE FLEET SWEEP COVERS HALF THE TESTS, AND THE REASON THE ONE FAILURE HID IS NOT THE OBVIOUS
ONE.** `tools/sweep_image_gates.sh` runs `ctest -LE host`, so the whole `-L host` half is outside
it and `tools/sweep_host_gates.sh` is that half. **BUT THAT IS NOT WHAT HID `sim_published_console`,
and reading it that way sends the next session after the wrong gap.** That gate is declared `image`
in `tests/static/test_classes.txt`, so `-LE host` SELECTS it and the sweep RUNS it; every image
sweep scored `sim-telem` at 28 image gates, 29 run, 0 failed. It failed in the COUNT PASS
afterwards, which ran the whole 296-test `sim-telem` suite in the sweep's own tree, and only that
run's `LastTestsFailed.log` named the arm. The sweep runs the image half ALONE under `-j1`, which is
the standalone condition these gates are documented to pass in; the count pass ran that gate beside
the other 295. So a green image sweep means the image gates pass SERIALISED, and says nothing about
the host half and nothing about the same gates batched. **Three instruments, not two.** The preset
is `sim-telem` and not `qemu-telem`: `sim_published_console` is registered only under
`KICKOS_ARCH STREQUAL "sim"`, so `qemu-telem` cannot run it at all.

**AN OPEN FLAKE, not dismissed.** `sim_published_console` failed once during a count pass and
passed eight runs after, five in isolation and three under the concurrent child-build load it is
most likely sensitive to. It configures and BUILDS a whole sim tree under `mktemp -d`, so in the
shared RAM-backed `/tmp` this file's own notes call a silent-ENOSPC hazard, and five such gates
run together under `-j8`. A mechanism is not a diagnosis and this one has none: **the failure
text was LOST because re-running overwrote `LastTest.log` before anyone read it. Read that file
BEFORE re-running a flake.**

**PR 7's MEASUREMENT HALF RAISED THE armv7m TELEMETRY FLOOR, AND THE OVERRUN IT WAS FIRST
WRITTEN UP AS DOES NOT EXIST.** Read the correction before the finding, because this file carried
the wrong version for two commits. The death path had an `EXIT` gate class on rv32imac and nowhere
else; declared on the other three, armv7m under `TELEMETRY_RTT` came out at frame 208 + depth 824 =
**1032 against a 960 `KICKOS_MIN_STACK_SIZE`**, which was written up as a floor-sized thread driving
privileged C 72 bytes past its own stack base. **IT CANNOT.** The 208 frame term prices a preemption
at the deepest byte, and at that depth none is possible: the 824 chain runs inside the `IrqLock`
`exit_current` holds across its `kickos_terminate` call (`sched.cc`), and on armv7m that lock is
BASEPRI at `PRIO_LOCK_BASEPRI` 0x20, which masks `PRIO_DEVICE` 0x30, `PRIO_SVCALL` 0xE0 and
`PRIO_PENDSV` 0xF0 alike. Frame and depth cannot coexist, so 1032 is a MODEL CEILING and not a
physical state.

**The floor leg is KEPT anyway, and the reason is about the model rather than the risk**: the gate
computes frame plus depth for every class on every arch, and exempting one because its deepest chain
happens to run masked would be a second truth about what a class means, maintained by hand, in a
file whose whole purpose is that a figure lives in one place. It costs `qemu-telem` alone. **What IS
true and was worth finding: `PENDSV`, at zone 100, is the ONLY thread-stack class enforced on that
preset** (`SVC` is skipped by `kstacks=0`, `SVCK` is a kernel class), so the armv7m floor had never
been tested under telemetry at all. And the leg must sit AHEAD of the plain armv7m one, because
**Kconfig takes the FIRST matching default and says nothing when a later one is shadowed**: the gate
caught that ordering error when it was written after.

**THE FRAME TERM IS NOT ZERO ON ANY OF THE FOUR, rv32imac's old `FRAME_EXIT 0` INCLUDED.**
Each stub is entered by an exception return, in thread mode, interrupts enabled, and its
`exit_current` reschedules, so a preemption below the descent is the ordinary case. armv7m 208,
armv6m 68, rxv3 **308**, rv32imac 128. The rxv3 figure is 300 plus the 8 bytes
`arch_fault_redirect_to_exit` skips below the block top so `kickos_rx_pendsw`'s block leg does
not read the USP as zero distance; the rv32imac one is the msip frame a reschedule puts below
the descent, its old 0 having been justified by the descent STARTING at the stack top, which
answers where it begins and not what lands beneath it.

**MANY FIGURES SIT AT EXACTLY ZERO SLACK, and that is the CONVENTION rather than a warning: an
enforced depth IS the fleet maximum for its class, so a class at its setter preset always reads
`n <= n`.** Do not count them as near-misses. `rx72m-st` `SYSK 788`, `EXITK 616`, `RET 476`;
`qemu-telem` `SVCK 1240`, `EXITK 944`, `RET 824`; `esp32c6-wroom-st` `EXITK 720`, `RET 368`;
`f302nucleo-st` and `due-st` `EXIT 568`. What IS a genuine near-miss is a BLOCK or FLOOR margin, and
there the tight ones are armv6m's block at 892 usable against an 892 `SVCK` zone, and the four-byte
pair this file already records on f302nucleo.

**THE BLOCK CANARY HAS NO READERS, so one advantage of moving anything onto it is LATENT.**
`kickos::kstack_canary_intact()` and `kickos::kstack_high_water()` are defined and declared and
called by NOTHING anywhere in the tree; `kstack_arm()` has one caller, in `kmain`. The canary is
laid at boot and never read back. So `check_trap_redzone.sh` telling its reader that the first
report of a block overrun "is a runtime canary failure on whichever board goes deepest" describes a
mechanism that cannot fire. Same class as a gate that printed `not enforced` and then enforced.

**WHAT IS OWED, and none of it is a surprise waiting to be found:**

  - **The Book page and `docs/reference/porting.md:1464`.** Deferred by the user, deliberately,
    until the whole PR suite is final: "because it is a work in progress I dont want to write
    down stuff that can changes later on". With every arch converted, no backend runs the
    mechanism `docs/book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md` describes, and
    its section arguing AGAINST a per-thread kernel stack now argues against what shipped.
  - ~~The ungated fault/slay exit path~~ **PAID.** All four backends now relocate the fault
    and slay stubs onto the dying thread's own kernel block, measured as `EXITK` against that
    block; `kickos_thread_return`, which nothing relocates, is measured as `RET` against the
    spawn floor; and armv7m carries `EXIT` for the four presets that carve no block. What is
    still owed here is narrower and is listed below: the poisoned-user-stack witness.
  - **No unit test for the canary/high-water machinery, AND NO READER FOR IT EITHER.**
    `kickos::kstack_canary_intact()` and `kickos::kstack_high_water()` are defined, declared
    and called by NOTHING; `kstack_arm()` has one caller, in `kmain`. So the canary is laid at
    boot and never read back, and `check_trap_redzone.sh` telling its reader that the first
    report of an overrun is a runtime canary failure describes something that cannot fire.
    Wire a reader before writing the test, or the test pins a mechanism nobody runs.
  - **No poisoned-user-stack witness for the death-path move.** It must use the SLAY path and
    not the fault path: a fault leaves the hardware exception frame on the dying thread's own
    stack by construction, so no fault can carry an intact-stack claim. The band has to be
    poisoned ABOVE the parked sp as well as below it. A written but never-built attempt is at
    `/var/tmp/kos-agent-faultsurvive.patch`, deliberately not landed.
  - **`close` INHERITS THE armv7m DUAL PATH**, not a deletion. Unlike rv32imac, the machinery
    stays while f302nucleo and due keep their red zones. **THE CONTINUATION-STYLE REWRITE IS NOT
    WHAT THAT NEEDS, and roadmap.md's "either a lower thread ceiling or continuation-style
    blocking" is now a false dichotomy**: a THIRD option shipped, which is armv7m carrying both
    entry designs under `KICKOS_KERNEL_STACKS`, and that board keeps its red zone and gains 204
    bytes of usable stack. What survives as a real residual is narrower and is recorded here
    rather than claimed closed: **on `f302nucleo`, `f302nucleo-st`, `due` and `due-st` a blocking
    syscall's continuation still rests on the USER stack**, those four being the presets where
    `KICKOS_KERNEL_STACKS` resolves 0. PR 7 is complete for `KICKOS_KERNEL_STACKS=1` and NOT
    universally, and the f302 continuation decision is deliberately NOT part of it.
  - **Two four-byte margins on one board.** f302nucleo's `DEPTH_SVC` 448 is EXACTLY its
    requirement once the STKALIGN pad is provisioned, and the panic-tail exclusion is
    load-bearing by four bytes. Both deliberate, both gated, neither slack.
  - `DEPTH_SVCK >= DEPTH_SVC` was written as a tripwire against swapping the two macros and is
    no longer one, 768 and 1240 against 448 being far enough apart that a swap fails the gate.
  - `genconfig.py` warns "set more than once" whenever a `-D` knob is re-passed unchanged,
    because it loads the live `.config` then the overrides. Noise in a channel documented to
    mean a declaration is wrong.

**THE OLD FRAMING, kept because the decision it records still holds:** M5.2.1 absorbs M5.2.2,
the mechanism is reworked once, in this milestone. Decided 2026-08-21, the user's words: *"Let's do M5.2.2 in M5.2.1.
We have no user, no need to push a fixed version ASAP. Let's just do the job once so I'll not
review a code that will change just after. BUT let's do that on a fresh session"*. So M5.2.1 is NOT
complete and must NOT be squashed or merged until the rework lands. The per-backend guard work
already on the branch is the INPUT to that rework, not the deliverable. Scope, the current
four-backend inventory, and the one hole still open are in
`~/.claude/projects/-home-leduc-projets-KickOS/docs/m5.2.1-trusted-context-plan.md`; read it
before touching `arch/*/switch.S`.

**AN EXTERNAL AUDIT FOUND TWO REAL PRIVILEGE ESCALATIONS. BOTH ARE FIXED, BOTH ARE WITNESSED ON
SILICON, AND ALL TEN CONFIRMED FINDINGS ARE DONE -- ON FOUR BRANCHES, NONE PUSHED.** A cold external
AI was given the repository and returned 19 findings; an adversarial pass verified each. 10
CONFIRMED, 8 PARTIAL, 1 not applicable, and only ONE was a convention collision.

  - **rv32imac `trap_entry` subtracted 128 from the INCOMING USER `sp` and stored 28 GPRs through
    it in M-mode.** No `mscratch` swap existed, no bounds test, and `pmp_cfg` never set the L bit,
    so M-mode bypassed PMP. Any unprivileged thread had a repeatable 112-byte fully-controlled
    write at any address.
  - **rxv3 wrote through the live USP and then `rte`d into `svc_trampoline` in SUPERVISOR mode with
    R0 = that USP**, so the whole kernel dispatch ran on an attacker-chosen stack. `pendsw` needed
    only a USP write and the next tick.

The tree ALREADY carried the right containment test, in `arch_fault_is_user_thread`, with the
comment "a thread that wrecked R0 would otherwise put privileged code on a stack of its choosing".
It guarded the FAULT path only, after the write had landed. **The lesson is not that the hazard was
unknown. It was named, in this tree, and guarded one step too late.**

**THE TWO OWED WITNESSES ARE TAKEN.** `esp32c6-wroom` and `rx72m`, tree `ff0d8469`, banners clean.
The C6 reports `RISC-V TRAP (illegal instruction)` with `mstatus=0x80` (MPP=U) and `rx72m` reports
`RX EXCEPTION (wild stack)` with `USP=0x8`, both with the witness intact. The absence of
`[trapwitness] CORRUPTED` is load-bearing and it is SOUND: the report runs ABOVE the dump header on
both backends (`arch_rv32imac.cc:653`, `arch_rxv3.cc:491`), so it is not truncation.

**F2 IS NOW DEMONSTRATED BY EXECUTION ON RX, WHICH THE VERDICT LISTED AS UNSETTLED.** Negative
control: the same tree with only the 12-line syscall-trap USP guard deleted (disassembly confirms
nothing else moved) dies silently after the banner, reproducibly, three runs, where plain
`faultsurvive` on that SAME unguarded tree runs clean end to end. The reason is in the link map --
`_kickos_trapstack_witness` sits at RAM `0x0` with the console TX object `g_tx_all` at `0x4`, so
the unguarded `userPC`/`userPSW` store lands on the witness AND the console ring at once.
**What the RX arm therefore CANNOT show is the `CORRUPTED` string itself**: the write that proves
the bug kills the console that would print it. Padding the witness apart was tried; the linker
keeps `g_tx_all` at `0x4`. The string is witnessed on `qemu-riscv` instead.

## Branches in flight, none pushed. All four merge cleanly onto master, cumulatively, in this order

  - **`sec/trap-stack`** the two criticals. sim 257/257, `qemu-riscv` 37/37, `qemu` 42/42, plus the
    two silicon witnesses above. The permanent arm is `faultsurvive_kwrite`.
  - **`rig/stlink-serial`** `KICKOS_VERSION` 0.5.1, `st-flash --serial` per board, the f411disco
    bench row, and `project()` made the single version authority (it said 0.4.7 while the banner
    said 0.5.1, and `PROJECT_VERSION` is what a consumer's `find_package` gets).
  - **`M5.2.1-audit-findings`** F8, F12, F5, F18. sim 277/277.
  - **`M5.2.1-audit-build`** F16, F17, F15, F10, F19b, F7, plus two approved class-closing gates.

Merged, the fleet is green: sim 293/293, `qemu` 52/52, `qemu-riscv` 44/44, and 18 to 21 on every
enforcing board including `rx72m`.

**THE `_appdata_fits` ASSERT HAD NEVER FIRED, ON ANY BOARD, EVER.** GNU ld completes layout before
it evaluates any assertion, so an app-data overflow died on `cannot move location counter backwards`
and the actionable message was unreachable in exactly the case it exists for. `. = MAX(., ...)`
keeps layout monotonic and the message now prints on all ten enforcing chips. **Do not re-derive
this by reading the scripts: the condition was always correct, the assert was simply never
reached.** 301 executables across ten boards were compared to prove passing links are unchanged,
and that comparison needs the build stamp held equal on both sides, `build_stamp.cmake` re-embedding
a timestamp and a `git describe` on every build.

**THE RX72M TOOLCHAIN BUILDS ON THIS BOX.** `$KICKOS_RX_TOOLCHAIN_BIN/rx-elf-gcc` works and
`rx72m-st` configures, builds, links and runs its gates; it is simply not on `PATH` even after
`env.sh`. A session already recorded "no rx-elf compiler here" and shipped a gate that failed that
board because of it. Check the env var before believing the toolchain is absent.

**THE 10-ANGLE REVIEW RAN, AND THEN AN EXTERNAL SECOND PASS RAN AFTER IT.** Both earned their
keep and neither was a formality. The 10-angle review found the first trap-stack fix still
exploitable at the low edge; the external pass found armv6m never closed at all, the RX syscall
zone not pricing the SWINT save, and F8's fix trading corruption for silent truncation. Its
verdict and this project's answer to it are section 8 of
`~/.claude/projects/-home-leduc-projets-KickOS/docs/external-audit-verdict.md`. **Read that
before re-auditing anything**, and note that two of its rows correct claims the same document
made earlier.


## What M5.2.1 was, and what it did not settle

Audit fixes, the f411disco bench on GlaDOS, and the banner -- all done. The plan, the 19 findings,
their verdicts and the audit's calibration are in
`~/.claude/projects/-home-leduc-projets-KickOS/docs/`: `external-audit.md`,
`external-audit-verdict.md`. The original M5.2.1 plan was retired 2026-08-21: every fact in it was
duplicated (the F1/F2 anatomy above, the verdict's sections 6 and 8.7 for the calibration, and
`CONTEXT.local.md` for the f411disco rig, which carries it more completely), and its one open item,
`imxrt1062`/`rp2350` missing the in-section appdata assert, is closed on all three scripts. The post-train backlog is in `deferred-after-pr-train.md`: the
driver-class validation hoist (SPI first, it has a runtime witness), four UART defects, nine SPI
divergences, and the fleet sensor witness now that the K64F carries an FXOS8700CQ.

**f411disco is benched and the banner is witnessed on silicon**: `1..105`, 105 ok, 0 skip, 0
partial, `mpu enforce`, `KickOS 0.5.1`, validated by hand through `check_tap_stream.sh`. The arm
count is DERIVED (86+14+1+3+1), not read back off the log. Its ST-Link is a V2 with no VCP, so the
console is the FTDI `BH001HMA` and NOT the probe.

**What the audit fixes are NOT witnessed by.** F8's numbers are derived from the code plus a host
gate that scores masked pushes, not measured on a scope: the claim is 4096 masked pushes down to 1,
a window independent of baud, not a measured interrupt latency. F5's re-publish path has a host arm
and NO sim arm -- adding one needs a second driver bring-up in the sim service list, which is new
service machinery rather than a gate extension. F18's wedged-console case has no image-level
witness at all, only the unit gate; producing one needs an app that publishes a console and never
returns to `kos_recv`.

**The ARMv6-M SVC hole is KNOWN-OPEN and deferred INTO the rework, deliberately.** Found while
trimming comments: `SVC_Handler` guards the PSP on one arm only, and `.Lsvc_slow` exception-returns
into `svc_trampoline`, which runs privileged in thread mode on that same unvalidated PSP. Reachable
by any syscall number other than 56. Full analysis, the four-backend inventory it has to unify, and
the saved partial fix are in `m5.2.1-trusted-context-plan.md`. A half-finished per-backend patch was
reverted on purpose so this milestone stays reviewable.

**No silicon witness is owed for it either.** Verbatim: *"honestly the witness I don't care: we are
going to redo the mechanism just after"*. picopi could carry one, so a later session will find it
missing and must NOT chase it. Emulator arms on `microbit`/`picopi-st` still count and still gate.
The rv32imac and rxv3 silicon witnesses stay VALID for this tree: those arches differ from their
witnessed trees in comments only.

**Read the audit's calibration before trusting any severity label**: it grades the shape of a
missing check, never whether the consequence reaches, and it cited no mitigating fact from
elsewhere in the tree even once. Its HIGH band carried almost no information; its MEDIUM band
carried four real new defects. Against our own known-defect list it found 1 of 7.


**I2C HAS A CLASS CONTRACT AND ONE ENGINE, AND THE ENGINE IS BUILD-VERIFIED ONLY.**
`user/include/kickos/driver/i2c.h` sits beside `spi.h` with the same four-symbol shape, and it
writes down FOUR REFUSALS: no late ACK decision, no hardware timeout, no address-versus-data NACK
bit, no bus-busy call. Each is a promise at least one of `mk64f`, `xmc4800` and `rx72m` cannot
keep. The third is CONDITIONAL: the discriminator is the POSITION of the NACK, so `xferred` is
mandatory and written before every return, and an implementation that reports 0 there on every
error withdraws the grounds the refusal stands on. The transaction deadline is a mandatory
SOFTWARE argument capped by `KOS_I2C_TIMEOUT_MAX_US`, because held SCL is the whole bus stopped
for every device on it. `KOS_EIO = 5` is new. `system/driver/rx72m/i2c_riic.cc` is the polled
RIICa master over one granted channel window; NOTHING IS WIRED TO THOSE PINS ON ANY BOARD HERE, so
it compiles and links and has no silicon witness. `kos_i2c_bus_config.irq` must be `KOS_CAP_NONE`:
RIICa's EEI is a grouped source whose IR flag lives in the kernel-reserved ICU, so a window holder
cannot retire a stale pending for it. The rate is SEARCHED against `achieved_hz` rather than
solved, and the reported rate substitutes the I2C specification's rise/fall maxima, so the real
bus is slower than the number, never faster.

**THE XMC4800 SPI REPORTED A SHIFT CLOCK 16 TIMES TOO LOW, AND IT IS UNRELATED TO THE I2C WORK.**
`achieved_hz` in `xmcssc/spi_usic.cc` divided by `(PCTQ+1)*(DCTQ+1)`, which the SSC baud generator
has no time-quanta counter for (RM 18.4.3.1): those two fields feed the slave-select delays of
eq.18.9, and only the ASC path next door is a time-quanta formula. `PSR_BUSY` is now marked ASC
mode only, `PSR[9:5]` being reserved in SSC and bit 9 being ACK in IIC.

**A KERNEL IS INSTANCE LOCAL, AND THE SIM HOSTS FIFTY AT ONCE.** `InstanceLocal<T>` in
`include/kickos/instance_local.h` is `KICKOS_MAX_INSTANCES` copies behind one index, and that
index is a literal `0u` by preprocessor unless `KICKOS_MULTI_INSTANCE` is set, which only the sim
may set. `struct Kernel`, the capability slab, the console TX ring, the fault record and the sim
backend state all wrap in it; module-private state stays in its own file rather than moving into
`struct Kernel`. Under the knob each instance owns a host thread, a `SIGEV_THREAD_ID` timer and a
`sigaltstack`, and `arch_shutdown` resumes the frame that started the instance instead of ending
the process. The `[n] ` stdout tag is emitted only where the process hosts more than one instance,
so a knob-on image running alone is byte-identical to a knob-off one and every banner-exact and
TAP-exact gate holds either way. `check_sim_multi_instance.sh` diffs fifty co-resident kernels
against a one-instance run of the same image; it does NOT cover a shared `sigaltstack`, which
needs an app that faults.

**THE CONSOLE RECLAIM IS SEQUENCED BEFORE THE EPIPE WAKE, AND THE MASK DOES NOT ORDER THEM.**
When the published console loses its last WAIT-bearing cap, `console_note_driver_death` and
`console_on_driver_death` run AHEAD of the loop that EPIPEs the parked senders. `sched::wake`
admits a switch for a closer that is neither exited nor dying, and `arch_switch` swaps INLINE on
the sim and on `lx6`, so a woken peer would otherwise observe a console still dark. The capsweep
arm `a_voluntary_close_reclaims_before_the_wake_it_admits` is the only oracle that fails on the
move.

**THE MILESTONE NUMBERS MOVED AND THE ABI FREEZE IS KEYED TO A NAME.** SMP is M6
(`docs/design-m6-smp.md`), the seam rework is M7, and the freeze is M8, the last milestone.
Prose says "the ABI-freeze milestone" rather than a bare number, so the next renumber does not
falsify it again. `TODO.md` M4.7.4 also records what the freeze itself owes: a full doc and
comment sync pass, because `check_doc_names.sh` validates a path and an UPPERCASE-prefixed
identifier and nothing else, and widening it was considered and REFUSED.

**`arch_cpu_id()` IS A PREPROCESSOR MACRO AT ONE CORE, NOT AN INLINE FUNCTION THE OPTIMISER
FOLDS**, because `-Os` has been measured out-lining an `always_inline` candidate in
`system/include/kickos/sys/atomic.h`. `KICKOS_NUM_CORES` is a Kconfig int, never prompted, 1 on
every board; at 1 the macro expands to `0u`, so the image carries no call, no symbol and no
branch. Above 1 it is a declaration with NO arch/common fallback, so a port that raises the knob
and ships no definition gets a LINK error rather than a kernel that believes every core is core
0. `cpu_id_fold` pins this and reads the GENERATED board config, not the CMake variable.

**THE SYSCALL PATH ANSWERS 64 BITS, SO `kos_clock_now` CANNOT FAIL.** `syscall_dispatch` returns
`uint64_t`; the source type decides the extension, so an errno arm sign-extends and every other
arm zero-extends, and the low half is bit-identical to what a 32-bit target always saw.
`arch_syscall` keeps its 32-bit return and no existing stub changes; `arch_syscall64` is a SECOND
LABEL ON THE SAME TRAP, the psABI's long-long return pair. `KOS_SYS_CLOCK_NOW` lost its
out-pointer along with the null, alignment and ownership checks that guarded it, and with them a
rejected store that returned a zero no caller could tell from a timestamp. Net smaller on every
32-bit family. `rxv3` and `lx6` are BUILD-VERIFIED ONLY.

**A WAKE INSIDE `endpoint_recv`'S SCAN PARKED THE WRONG THREAD, AND THE FIX IS HERE.** `kos_call`
over an info-less recv could return a byte count where the ABI promises `-KOS_ENOSYS`, letting the
caller read another sender's payload as its reply. `switch_to` advances `kernel().current` BEFORE
`arch_switch` and every pending backend returns from `arch_switch` at once, so a `sched::wake`
inside the scan moved `current` onto the woken caller and the `wq_block` below parked THAT caller
on `recv_waiters` instead of the server. `wake` splits into `wake_no_resched` and
`resched_after_wake`, the scan readies without rescheduling, and one reschedule is deferred to the
highest-priority thread woken on each exit that does not park. The regression arm is selftest 40
`call_infoless_revert`, restaged so `recv#2` always finds the caller alone and parks; `qemu`
`mps2-an386` PENDS its switch and catches it, `sim` swaps inline and cannot witness the class at
all. **The bug is on `master`, not introduced by M5**, and branch
`hotfix/wake-parks-wrong-thread` is retired.

**`KOS_SYS_CALL_REG = 56` EXISTS AND EMITS NO CODE ON THIS BRANCH.** The register-carrying call
number, `KOS_CALL_REG_FALLBACK` and the five-word / 20-byte payload budget are ABI here; the
trap-handler implementation is a LATER PR. `kos_call`'s register arm and `arch_syscall_reg` are
both behind `KICKOS_ARCH_HAS_IPC_FASTPATH`, which no backend defines yet, so the arm compiles
away entirely and the generic dispatch answers `KOS_CALL_REG_FALLBACK` rather than implementing
the register form. Dead ABI on purpose: reviewing it apart from the fastpath is the point.

**THE IPC BASELINE EXISTS AND IS MEASURED.** There was no call/reply figure anywhere before this;
there is one in `docs/design-m5-ipc-fastpath.md`, and anything touching the call path should read
that page before touching it. A round trip is a FIXED term plus a per-byte term, and the fixed
term holds within 1.5 percent across three ISAs and three clocks, which makes it an instruction
count rather than silicon or memory.

**THE BENCH APP WAS BROKEN AND NOBODY KNEW, WHICH IS WHY THERE WAS NO BASELINE.** Its helpers were
DIRECT kernel calls, so with root unprivileged on every board with a ring the first call faulted
and killed the reporter: only `esp32-wroom`, which has no ring at all, ever ran it. It also never
returned from `main`, so `KICKOS_SHUTDOWN_TO_BOOTLOADER` was inert for it. Both are fixed behind a
new `KOS_SYS_BENCH` syscall, and the app now returns after a bounded number of reports.

**`CALL_MINT` SPLITS 290/109 INTO A CAPABILITY HALF AND A USER-MEMORY-WRITE HALF.**
`CALL_MINT_CAP` is `cap_install_reply` at 290 cycles, `CALL_MINT_INFO` is `write_recv_info` at
109. They sum to 399 against the pre-split 400, and twelve other leaves in the same capture are
byte-identical, so the split is placed honestly. The 290 is NOT a search: the free-list peek
returns on its first test and the unlink beside it is O(1). It is mint machinery, which is what a
reserved per-thread reply slot would remove.

**THE INSTRUMENT'S DOCUMENTED CORRECTION RULE WAS WRONG, and this is the bigger finding.**
`bench.h` said a phase is `(phase - k * PH_NULL)`. The closing timestamp is evaluated as an
ARGUMENT, so `PH_NULL` measures two counter reads and reads 1 cycle; but a bracket NESTED inside
an enclosing span charges that parent the two reads PLUS the whole accumulator call, about 57
cycles. The rule therefore understated the per-bracket charge by roughly 56x, every COMPOSITE was
inflated, and only the LEAVES were ever honest. FIXED with a proper nested control phase: an
EMPTY bracket cannot price a NESTED one, so the second control had to exist and be validated
differentially rather than assumed.

**A COMPOSITE SHARED BETWEEN TWO CODE PATHS WAS REPORTING THE WRONG PATH'S MINIMUM.** One
accumulator served both the fastpath and slowpath arms of the call, so the shorter arm's body set
the minimum the longer arm was read for. There is now one accumulator per code path, and each
closes INSIDE its own arm and inside the lock.

**THE ROUND TRIP HAS THREE LOCKED LEGS, NOT TWO.** The server's own `kos_recv` park holds
`IrqLock` across one of the two context switches per round trip, and nothing had ever bracketed
it. A leg that is never bracketed is not a small error in the total, it is absent from it.

**THE LOCKED FRACTION IS 53 PERCENT, with a 43 PERCENT LEAF FLOOR**, re-derived on
`esp32c6-wroom` with the corrected instrument. That Amdahl-bounds a two-core big lock at
**1.31x**, and the floor caps it at 1.40x. It REPLACES the unproven "about 2x" the SMP spike
assumed, and the gap between floor and direct is itemised rather than left as a residual. Since
that number is what sizes the M6 big-lock decision, per-core run queues and finer locks are not a
later optimisation but where most of the payoff actually is.

**THE COPY PATH IS 3.6x FASTER (`topic/wordwise-memcpy`, not merged).** 10.0 to 2.75 cycles per
byte. A 256-byte round trip goes 13883 to 9555 cycles, **31 percent**; 8 bytes goes 9.7 percent. The
fix was NOT libc: `ep_copy` and the `kaccess_*` pair are private byte loops in `syscall_mem.cc` and
`memcpy` was never on the measured copy path, so deleting or rewriting libc would have moved zero
cycles. The word path is gated on `(dst ^ src) & (WORD-1)` and a two-word minimum, so no target ever
sees an unaligned access and `-mno-unaligned-access` is satisfied without per-ISA divergence.

**Two things that fell out.** `MPU_APPLY` dropped 452 to 277 cycles per switch because
`arch_mpu_apply` emits a `memcpy` per region, so context-switch throughput rose 11.7 percent for
free. And `lib/CMakeLists.txt` needs `-fno-tree-loop-distribute-patterns` on that file: the word
loops ARE the memcpy idiom, and the pass that recognises it would rewrite each into a call to itself.

**A cost and a caveat.** Fixed cost per copy rose 14 to 49 cycles, so a copy under about five bytes
is now slower; the 8-byte IPC case improved. And `wcase-irq[1024B]` did NOT move, correctly: it
measures the bench harness's OWN inline byte loop, not `ep_copy`. That row now overstates the
kernel's longest copy by 3.6x.

**ONE ATOMIC MECHANISM TREE-WIDE, AND THE ORDER IS CARRIED IN THE TYPE.** `<atomic>` now appears
in EXACTLY ONE FILE, `system/include/kickos/sys/atomic.h`. The C-facing atomic macros are gone;
`kos_uart_stats` is nine plain words behind `kos_counter_*`, a ONE-MEMBER STRUCT, so `++`, `+=`, a
bare read and a bare write are COMPILE errors in C11 and C++20 alike. The type enforces it, not a
gate and not a convention. `Order` is a BITMASK, because acquire and release order OPPOSITE
accesses of the same field and every cross-thread word here needs both; ACQUIRE and RELEASE are
placed at the residues rather than swept on tree-wide. The byte ring's publication-barrier macro
is DELETED: head and tail carry the ordering in their own type, so there is no consumer `-D` left
to get wrong.

**`Thread` SHRANK 264 TO 256 BYTES ON armv7m.** `switch_count` narrows to 32 bits and moves into
the padding hole before `deadline_ns`, so the field is free: it costs bytes that were already
being paid. Measure `sizeof` before and after rather than reasoning about field order.

**TWO BOARDS CANNOT MEASURE CYCLES AND ONE CANNOT MEASURE TIME. Do not spend a pass rediscovering
this.** `xmc4800-relax`'s DWT CYCCNT is DEAD, every delta zero over 120000 samples, so that board
is wall-clock only. `esp32-wroom`'s monotonic clock intermittently returns EQUAL values across
hundreds of milliseconds, and instrumenting it made the failure rate RISE, which is what makes it
a race rather than a resolution limit; its cycle figures are fine. `teensy41` is the board that
gives the full nested decomposition, having a proven-live DWT and a switch that PENDS, so unlike
the LX6 its composite spans are honest.

**WITNESSED HERE:** `sim` 242/242, `qemu` 42/42 and `qemu-riscv` 36/36, all green, every static
gate exits 0 and `dash_punct` is one of them. Each denominator is one above `M5.1.6`'s, the new
gate being the one added test. `teensy41`, `rx72m`, `esp32c6-wroom` and `microbit` build. No
silicon run belongs to this tree.

**THE GATE AND ITS SWEEP SHIP TOGETHER, and that was the decision.** `dash_punct` found 196 lines
on `master`; four of them are real shell option terminators, which the gate's separator clause now
recognises, and the other 192 across 107 files are rewritten here. Landing the gate on its own
would have handed `master` a red test with no owner.

**`pizero2350` IS RE-WITNESSED AT `ce34ac66`, BOTH ARMS, and it is the only board reachable
without an operator** (BOOTSEL in, `KICKOS_SHUTDOWN_TO_BOOTLOADER` out, so the loop is unattended).
`usbcdcwit` twice: `accepted=8192 of 8192 err=0`, `drop=0`, PASS, banner `ce34ac66` clean.
`selftest` twice: arms `10..104`, 95 captured, 0 not ok, 0 skip, 0 partial, validated by hand
through `check_tap_stream.sh` with `TAP_HEADLESS_LAST=104` DERIVED from
`user/apps/common/selftest/CMakeLists.txt` (86 + 14 + 3 + 1).
**The selftest arm carries NO banner and its tree attribution is therefore WEAKER than the
usbcdcwit arm's**: the suite floods immediately after the banner, so the USB CDC head loss eats it
every time, where `usbcdcwit`'s slower start lets it through. Both captures in a run come from one
build, so the attribution is sound, but it rests on the sibling capture's bytes and not its own.

**`maxzero` SHIFTED, 569 DOWN TO A 542-549 BAND, AND IT IS NOT THE EXACT INSTRUMENT M4.9.1 TOOK
IT FOR.** Five samples: 569 and 569 before `b40fbefc`, then 549, 549 and 542 after, the last two
at trees whose only difference is a COMMENT, so the image is the same across them. The two
clusters do not overlap, so the downward shift is real and `b40fbefc`'s relaxed-atomic counters
in each driver's IRQ path are what moved it. But the value has a run-to-run spread of at least 7,
so "an identical `maxzero` shows the conversion is inert" was never a sound test, and calling it
stable per TREE on two samples overstated it the same way. What holds is the weaker and
sufficient claim: `drop=0` and `accepted=8192 of 8192` on every run, so nothing is lost, and the
ring stays full for a measurably different span. `wakes` and `spurious` carry nothing.

**TWO ARMV7M FAULT-PATH DEFECTS ARE FIXED HERE, and the reason they are not deferred is that the
fleet re-witness was ALREADY owed.** Deferring them to protect witnesses that `b40fbefc` had
already invalidated would have bought nothing.
- `kickos_arm_mpu_program` no longer zeroes `MPU_CTRL`. That also stopped the chip FIXED rows
  applying, and on `imxrt1062` those carry the ERR011573 anti-speculation wrap over the FlexSPI
  band the code is itself executing from. Each per-thread descriptor is disabled individually
  before its base moves instead, and the half-updated set is unobservable because only privileged
  handler code runs until the exception return. `fixed_init` still zeroes it, where no fixed row
  exists yet to lose.
- `SHCSR_BUSFAULTENA` is set, so a bus abort no longer escalates, and the reporter labels a set
  BFSR byte `BUS FAULT`. Safe because `core_vectors.inc` already points all four fault vectors at
  the same reporter.
**Witnessed on both PMSA generations:** `teensy41` `selftest` 1..104 all ok and `pizero2350`
(PMSAv8) arms 10..104, both clean at `4f8d6ab2`, plus `teensy41` `rootfault` still denying a
cross-domain write as a clean MemManage (`CFSR=0x82`, `ADDR=0x20244000`). **NOT witnessed: the
`BUS FAULT` label itself**, which prints only on the panic path, while every bus fault this
milestone produced was in an unprivileged driver thread killed thread-scoped.

**THE WITNESS LEDGER AT HEAD, which is the first thing to read before crediting anything below.**

| board | state at HEAD | why |
| --- | --- | --- |
| `teensy41` | **CURRENT** | `selftest` 1..104 and `rootfault`, both at the MPU commit |
| `pizero2350` | **CURRENT** | `selftest` 10..104 at the MPU commit |
| `teensy41` USB CDC, `teensy41` `reclaimwit_drain`, `pizero2350` `usbcdcwit` | **STALE** | taken BEFORE the MPU commit, which changes every armv7m image |
| `picopi` `frdmk64f` `rx72m` `xmc4800-relax` `esp32-wroom` `esp32c6-wroom` `f302nucleo` | **STALE, OWED** | all predate `b40fbefc`; none was on the bus this session |

**THE MPU COMMIT SHIPPED A REGRESSION AND THE WITNESSES COULD NOT HAVE CAUGHT IT.** Dropping the
trailing `MPU_CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA` from `kickos_arm_mpu_program` rested on
`kickos_arm_mpu_fixed_init` having enabled the MPU already. That function is called from ONE chip,
`imxrt1062`. On every other PMSAv7 chip the MPU was then never enabled at all while the banner
still printed `mpu enforce`: mps2, stm32f411, rp2040, xmc4800. PMSAv8 was unaffected, keeping its
own enable in `arch_arm_pmsav8.cc`.
**The two boards witnessed are exactly the two immune to it** -- `teensy41` calls `fixed_init`,
`pizero2350` is PMSAv8 -- so a green fleet pass on those two said nothing. What caught it was the
new `-LE host` image-gate sweep, on `qemu_mpu_fault`, `qemu_rootfault` and `qemu_faultoverflow`,
three gates no automation had ever run. The trailing write is restored, and it only SETS bits, so
unlike a leading `MPU_CTRL = 0` it never stops the fixed rows applying. All four MPS2 presets now
pass their whole image half (23, 22, 23, 23 gates).
**The lesson is about coverage, not about the line:** the three silicon boards in the blast radius
have no automated image gate at all, so only a bench visit or that sweep could ever have found it.

**THE SURVIVING WITNESSES WERE PROVEN, NOT ASSUMED.** Everything committed after the MPU commit
is comment-only for an armv7m image except `regs/aipstz.h`, whose change is `static_assert` lines
alone. Checked by disassembling `chip_imxrt1062.cc.obj` at both trees: 345 instructions,
byte-identical. A capture taken BEFORE that commit is stale on every armv7m board, since
`arch_arm_common.cc` is in all of them.

**`teensy41` NO LONGER NEEDS AN OPERATOR**, which changes what a future pass can do unattended:
`arch_reboot`'s `bkpt #251` is caught by the MKL02 companion and the board re-enumerates as
HalfKay by itself, so `tools/bench/bench.sh` now gives it `KICKOS_SHUTDOWN_TO_BOOTLOADER` for the
reason the RP boards have it. Two consecutive captures were taken with no button press.
**`pizero2350` LEFT THE USB BUS after its third run** and answers as neither BOOTSEL nor a CDC
console, so it wants a power-cycle before it is reachable again. Its witnesses were taken first.

**EVERY OTHER CAPTURE BELOW IS STALE, AND READ THE REST OF THIS FILE WITH THAT IN FRONT OF IT.** All six
witness-recording commits precede `b40fbefc`, which rewrote the `kos_uart_stats` counter accesses
inside the IRQ path of every driver -- `rpusb.cc`, `uart_lx6.cc`, `uart_c6.cc`, `uart_k64.cc`,
`uart_sci.cc` -- plus `user/src/console_ring.cc` and `user/include/kickos/sys/uart.h`. That is
shipped code, so a witness taken before it describes a different image and the paragraphs below
over-claim. **They are deliberately NOT retaken yet**: the RT1062 MMIO-grant defect is still open,
and a fix landing in the shared armv7m MPU path rather than in `imxrt1062` alone would move the
image for every armv7m board and throw the pass away. One pass, at the final tree, after that fix.

**The codegen claim was MEASURED, not trusted, and two of its corollaries were false.** A relaxed
32-bit load and store are the same single instruction as `volatile` on all five backends. But a
64-bit relaxed load is a `__atomic_load_8` LIBCALL on every backend including armv7m, and a
freestanding link has no libatomic -- so the 64-bit cross-thread fields stay `volatile` and say why
at the site. And `is_always_lock_free` is **0** on armv6m and rxv3 even where the load and store ARE
inline plain instructions, because RMW is not, so no `static_assert` may rest on it.

**`picopi` IS RE-WITNESSED AT THE FINAL TREE, TAG `m492b`**, `dcd5e21f`, clean rather than
`-dirty`: `kickos_services_picopi_usbcdc`, over the board's own USB CDC with no cable, arms
**20..104**, 85 lines, 0 not ok, 0 skip, 0 partial. Validated by hand through
`check_tap_stream.sh` with `TAP_HEADLESS_LAST=104`, the 104 DERIVED from
`user/apps/common/selftest/CMakeLists.txt` (86 + 14 + 3 + 1) and never read off the stream.
**Arms 1..19 are outside the verdict by construction** -- a console that IS the device cannot
deliver its own head -- and the checker prints that itself. The board returned to BOOTSEL unaided.
Log `.session/logs/m492b-picopi-selftest.log`.

**`m492` is the SUPERSEDED capture of the same board** and its log is kept. It banners `10175a7d`,
which is the atomics commit BEFORE the console seams landed, and `chip_rp2040.cc` gained three
functions after it -- so that witness stopped describing the tree and was retaken rather than
carried. Where each capture starts differs (21 versus 20) because the head loss is a race with USB
enumeration, not a fixed cut.

**`pizero2350` IS PAID, BOTH WITNESSES, at `f536d084`** -- the board was swapped in for the picopi
mid-session. `usbcdcwit` REPRODUCES THE M4.9.1 FIGURES EXACTLY: `accepted=8192 of 8192 err=0
maxzero=569`, `drop=0`, PASS, where M4.9.1 at `e0ab9cf9` recorded 8192 of 8192, `drop=0`,
`maxzero=569`. The identical `maxzero` is the load-bearing part: the ring goes full and the
short-accept retry recovers it in exactly the same shape after every index in that ring became a
relaxed atomic, so the conversion is observably inert on the path that stresses it hardest.
`tx`, `wakes` and `spurious` have NO M4.9.1 baseline, so read nothing into `spurious=1`.
Its `selftest` under the same list is arms 10..104, 95 captured, 0 not ok, 0 skip, 0 partial --
and that image LINKS AND BOOTS the new rp2350 reclaim body, which retires the risk that the body
breaks the board without being a witness of the reclaim firing.

**BOTH ESP BOARDS ARE WITNESSED UNDER THEIR `_uartirq` LISTS at `32470528`**: `esp32-wroom`
`1..100` and `esp32c6-wroom` `1..104`, 100 and 104 ok, 0 skip, 0 partial, the C6 enforcing. Both
defaulted to `kickos_services_none`, so the DRIVER being in the image is what makes the run
non-vacuous, and both streams say so themselves: `[lx6uart] device up (IRQ TX/RX)` and
`[c6uart] device up (IRQ TX/RX)`, each with `# tap route: stdout endpoint -> console driver`.

**CLOSED: A RECLAIM FIRING AND A TERMINATE DRAINING ARE BOTH WITNESSED ON SILICON**, by
`user/apps/common/reclaimwit`, on `teensy41` at `21306644`, log
`.session/logs/m492tr-teensy41-reclaimwit_drain.log`. Counted on the EXACT emission strings and
not on the word, because the app's own reading key contains it four times:

| discriminator | wanted | got |
| --- | --- | --- |
| `MUTE kernel console while the driver holds it` | 0 | 0 |
| `LIVE kernel console after the driver died` | 1 | 1 |
| `routed through the driver` | 0 | 0 |
| `DRAINTAIL ... <<<DRAIN-END>>>` | intact | intact |

plus `slay rc=0` and a post-death send of `-32` (`-KOS_EPIPE`). The sink driver writes to no
console on any board, so the LIVE bytes can only have come from `arch_console_write_sync` in
`RECLAIMED`; and the sentinel survives, so `arch_console_flush_sync` held the core until the shift
register emptied. The paragraph below is what this replaces, and is kept for the reasoning.

**WHAT NO CAPTURE IN THIS MILESTONE WITNESSED UNTIL `reclaimwit` EXISTED: a reclaim FIRING or a terminate DRAINING.**
The seven selftests prove the four new console-seam bodies link, boot and leave their drivers
working. They cannot prove more, and the reason is a missing instrument rather than a missing
run: **`drvdeath` is a SIM app**, sequenced by `KICKOS_SIMCON_EXIT_AFTER=1` in
`system/init/sim/service_list.cc`, and the only silicon driver-death prober in the tree is
`user/apps/xmc4800-relax/conreclaim`, which is board-specific. So `esp32`'s reclaim-window fix --
a real defect, where the reclaim could fire while the IRQ thread still held UART0 -- is argued
from `dev_window_free`'s overlap test and witnessed only as far as "the board still boots".
Closing this properly wants a BOARD-AGNOSTIC driver-death prober, the way `faultsurvive` is the
board-agnostic fault prober. That is its own piece of work and it is what the four flush_sync
bodies need too.

**`teensy41` IS PAID TOO, TAG `m492t`**, once an FTDI went onto pin 1: `1..104`, 104 ok, 0 skip,
0 partial, enforce, banner `aafb143f` clean, validated by hand. The first HalfKay load failed and
the retry took it, which is this board's documented behaviour and is handled automatically.
**So all three stale M4.9.1 captures are retaken**, and EIGHT boards across five ISAs witnessed the
tree AS IT STOOD -- see the staleness note at the top before crediting any of them to HEAD.

**THE RT1062 USB BACKEND STOPS MAKING PROGRESS, AND WHAT STOPS IT IS OPEN.** `m492te`,
banner `7040afe7`, log `.session/logs/m492te-teensy41-selftest.log`: the kernel reads USB1's
read-only `ID` at `0x402E0000` and gets `0xE4A1FA05`, its reset constant, on the same line that
shows CCGR6's usboh3 gate on, PLL_USB1 locked and unbypassed, and the PHY fully powered. The
unprivileged driver thread's breadcrumb stops at `STAGE_PROBE`, which is set immediately before
that same read. So the bus, the clock and the PHY are out.
**ROOT-CAUSED ON SILICON: THE AIPSTZ BRIDGE, NOT THE MPU.** `m492tg` prints
`CFSR=0x8200 ADDR=0x402e0000` -- BFSR `0x82`, PRECISERR plus BFARVALID, with a `0x00` MemManage
byte. A precise BUS fault at the USB1 base is what `OPACR`'s Supervisor-Protect does to an
unprivileged access; an MPU denial would have set DACCVIOL and MMARVALID instead. The MPU window
is programmed and innocent.
**FIXED AND WITNESSED: the backend WORKS.** `m492tw`, arms 12..104, 93 captured, 0 skipped, 0
partial, over the board's OWN USB CDC with no cable, unprivileged and under MPU enforcement.
`arch_periph_enable` gained a USB1 entry that clears the slot's supervisor-protect bit, and the
slot's remainder (OTG2, USBNC) went into `arch_reserved_blocks`.
**The bus gate and the MPU are INDEPENDENT barriers, and treating them as one cost a wrong
write-up.** This file previously called the coarse AIPS slot a policy crisis needing a decision.
It is not: the MPU stays the fine-grained authority, the driver's window is still 512 B, and what
the containment rule protects is a coarse gate over KERNEL-RESERVED registers. Reserving the
remainder satisfies that a second way, so `arch.h` now states both ways instead of only
containment. No policy was relaxed.
**Three confident inferences in this chase have been wrong, none caught by a gate**, the last
being mine: that no fault was involved, argued from two silences that are both DESIGNED. A
published console drops kernel writes including fault reports, and `drv::bring_up` publishes
before it spawns; a dark LED rules out a panic and nothing else. `TODO.md` keeps all three.

**THREE MORE BOARDS ARE WITNESSED AT THIS TREE, and two of them reopen coverage that had none.**
All three banner `commit c8671356` clean, all three validated by hand through
`check_tap_stream.sh` against a count DERIVED from `user/apps/common/selftest/CMakeLists.txt`.

| board | tag | list | result |
| --- | --- | --- | --- |
| `rx72m` | `m492r` | `kickos_services_rx72m_uartirq` | `1..104`, 104 ok, 0 skip, 0 partial, enforce |
| `frdmk64f` | `m492k` | default (polled `k64uart` + DSPI) | `1..104`, 104 ok, 0 skip, 0 partial, enforce |
| `picopi` | `m492b` | `kickos_services_picopi_usbcdc` | arms 20..104, 0 not ok, 0 skip, 0 partial |

**`rx72m` is the one that matters most**: no emulator and no CI gate anywhere, so silicon is the
only check that exists for the rxv3 atomics conversion, its clock anchors and the
`arch_irq_inject` lock fix. Its image carries the new reclaim body too, `rxsci` being linked
under that list.

**`frdmk64f` CLOSED THE TWO HOLES ITS OWN FLEET RULING OPENED**, because a person was present to
clear the once-a-day OpenSDA licence dialog. The segmented capability table is exercised, and the
proof is by-name rather than by count: `cap_chunk_span` and `cap_child_width` report a bare `ok`
here, where `microbit` reports `ok ... # PARTIAL table is 7 slot(s): the flat decode, no index
reaches the granule`. A PARTIAL is an arm that ran its invariant and left a sub-case unreached, so
its ABSENCE is what says `KCAP_RUN_CHUNKS > 1` was decoded. The same capture is the SYSMPU
enforcement class. Both are listed below as having no instrument; at this tree they have one, and
it needs a human each calendar day.

**The `picopi` witness was PROVEN to still describe HEAD rather than argued to.** It was taken at
`dcd5e21f` and three commits landed after it. Rebuilding the same image at the same build PATH at
HEAD leaves exactly 11 differing bytes, every one inside the two copies of the embedded commit
string, with `text`/`data`/`bss` identical. Rebuilding at a DIFFERENT path instead shows a 4-byte
`text` delta and 58 normalised instruction differences, all of it `tests/tap/tap.h`'s `__FILE__`
shifting the literal pools -- which is why the same-path rebuild is the test and a normalised
`objdump` across two paths is not.

**THE 51-PRESET HOST SWEEP IS GREEN AT THIS TREE**: `DONE 51 preset(s): 51 pass (0 reused),
0 fail`. `0 reused` is load-bearing, `SWEEP_FORCE=1` having made every preset re-run rather than
be skipped as already-passed. `sim` and `sim-telem` both register **208**, which is M4.8.4's 204
plus exactly the four new gates, so the K-seam still links under `sim-telem` and the new gates
registered fleet-wide rather than on the sim alone. The board spread is 12/13/14 and is ACCOUNTED
FOR, not tolerated: 12 is the base, `kernel_ctor_placement` adds one on armv7m AND MPU,
`riscv_no_smalldata` adds one on the RISC-V presets, `oot_export_mcu` adds one on the qemu family,
and `qemu` alone qualifies for two. Checked the inverse way too -- of the presets that qualify for
the ctor gate, none is missing it. `qemu-telem` looks like a miss and is not: it is
`CONFIG_KICKOS_HAVE_MPU=0`, so it correctly has no ctor gate and reaches 13 by `oot_export_mcu`.

**EVERY CHIP THAT PUBLISHES A CONSOLE NOW CARRIES ALL THREE SEAMS**, where four of seven carried a
partial set and three carried none. `arch_console_reclaim`, `arch_console_reclaim_window` and
`arch_console_flush_sync` are complete on mk64f, xmc4800, esp32c6, esp32, rp2040, rp2350 and rx72m;
`stm32f302` stays flush-only because no userspace console driver exists for it. **All of it is
BUILD-VERIFIED ONLY** -- `nm` plus the link map proving the weak `arch/common/` member is no longer
extracted, and `check_seam_defaults` -- and `TODO.md` records the witness each one owes. Only
`picopi` of the seven is on a bus here.

**The reclaim window invariant is ONE-WAY**, and this is the thing to not re-derive wrongly: the
window must COVER what the reclaim WRITES, which each chip's `static_assert` enforces locally, and
it does NOT have to match the service-list grant. `dev_window_free` tests OVERLAP, so a holder able
to reach any register the reclaim writes necessarily overlaps the window. It reads like a coupling
that wants enforcing across `arch/` and `system/init/`; it is not one.

**`microbit`'s arena GAINED a granule here, which is the direction nothing checks automatically.**
`.bss` 6528 -> 6512 and `_ebss` == `__kickos_ram_start` == `0x20001b00` again, where they had drifted
16 bytes apart. One symbol moved: `g_hog_until` (8 B) out, `g_hog_start_ns` (4 B) in. The skip set
was therefore diffed BY EYE as this file requires: **18 reported, 18 declared, exact match**, so the
extra grain freed nothing. `microbit_selftest` 24/24.

**M4.8.4 IS MERGED (PR #22), and it closes the tail of the three milestones before it.** Classes A,
B and C are all done -- the six latent defects, the three accepted costs (FIXED rather than accepted:
the kill/slay ABI is what came out of that), and the instruments that let them through. **Everything
through M4.8.4 is merged, so anything found against any of it from here is a MISS and gets filed as
one, not folded back into a milestone.**

**THE MERGED TREE IS WITNESSED ON SILICON, TAG `m485sq`, and the witness belongs to `master`.** All
eleven captures carry `3e35aaee` with no `-dirty` (nine as `commit 3e35aaee`, the two `f302nucleo`
streams as the terse `c 3e35aaee`) -- and that commit's tree is `87ca4c5d`, BYTE-IDENTICAL to the
merge commit's. A witness is valid for a TREE, so this one survives its branch tip being unreachable:
check the tree, never the hash. Ten of the twelve service lists `bench-fleet.sh` derives were run,
zero failures, and all eleven streams reconcile:

| board | class | service list | plan | result |
| --- | --- | --- | --- | --- |
| `xmc4800-relax` | PMSAv7 | `kickos_services_xmc4800relax`, then `_console`, then `_uartirq` | `1..104` x3 | 104 ok on all three |
| `rx72m` | RX MPU, no CI gate at all | `<default>`, then `kickos_services_rx72m_uartirq` | `1..104` x2 | 104 ok both |
| `esp32c6-wroom` | PMP NAPOT | `<default>`, then `kickos_services_esp32c6_uartirq` | `1..104` x2 | 104 ok both |
| `esp32-wroom` | LX6, no unit | `<default>`, then `kickos_services_esp32_uartirq` | `1..100` x2 | 100 ok both |
| `f302nucleo` | ring-only | `<default>`; NO provider exists for this board | `1..51` + `1..49` | 51 + 49 ok |

**`frdmk64f` IS OUT OF THE BENCH FLEET, BY RULING RATHER THAN BY ABSENCE, so a fleet pass now reports
INCOMPLETE BY DESIGN and can never read fully green.** Its two lists are the two NOT RUN, and the
reason is the SEGGER OpenSDA licence model rather than anything about the board or the port: an
unattended pass cannot clear a once-a-day dialog, and the cost of working around that was ruled not
worth paying. **Read `INCOMPLETE` on this fleet as the expected result, and read the per-list table
instead** -- the exit status is non-zero by construction, so treating it as a failure signal will
mislead every future pass. The board stays a supported port and a fleet BUILD target; what it no
longer has is a route to silicon. **One thing now has no instrument at all**, the SYSMPU
enforcement class. The segmented capability table was listed here as a second; that was BACKWARDS
and is corrected below.

Behind it, **M4.8.3 (PR #21)**: the task layer -- a task is a set of threads that dies as one unit,
the address space stays on `Domain`, and the group gate is CREATORSHIP rather than possession. It
also carries the two things its own captures found -- `rxv3` fault isolation, and a published console
no longer swallowing the fault record.

Behind it, **M4.8.2 (PR #20)**: the host unit-test layer, GoogleTest via Conan behind
`find_package(GTest QUIET CONFIG)` so vcpkg or a distro package satisfies it too, per-case ctest
entries under the `host` label, and the two substitution seams (U-seam at `extern "C" kos_*`, K-seam
at `arch_*` with a whole-`Kernel` reset). It is the tool that proved the `sched::wake()` dying-guard
repair that shipped with it.

Behind it on `master`: M4.8.1 (PR #19) -- one generic driver service replaced twelve
per-(class x chip) bring-ups, and `driver_bringup.h` is gone; M4.7.9 (PR #18), M4.7.8 (PR #17),
M4.7.7 (PR #16), M4.7.6 (PR #15), M4.7.5 (PR #14), M4.7.4 (PR #13), M4.7.3 (PR #12), M4.7.2 (PR #11),
M4.7.1 (PR #10), M4.6.1 (PR #9), M4.5.9 (PR #8), M4.5.8 + M4.5.7 (PR #7), M4.5.6 (PR #6).

**Three defects that predate the driver work were fixed alongside it**, and each is the kind that
only silicon or a sanitizer finds:
- **PendSV read `g_arch_next` and the deferred MPU stash unmasked** on both ARM ports. The pair is
  written together under the kernel lock, so a device IRQ between the two reads resumed a thread with
  another thread's regions programmed. It presented as a USB CDC hard fault on `picopi` about 75% of
  runs, and the fault frames were FOSSILS of an earlier SVC, which is why the dump read like an
  escalated `svc`.
- **Three selftest IRQ arms hardcoded NVIC line numbers with no per-chip free-line set.** Inert on
  QEMU, wired peripherals on RP2040. `KICKOS_IRQ_SOFT_ONLY_BASE` lets a chip declare the fact.
- **`thread_join` read its start time after the spawn**, so a higher-priority target could reach its
  sleep first and the measured interval no longer contained it.

**Two arms filed as unreliable were not flakes.** `rr_interleave` was a symptom of the PendSV race
(4 failures in 6 runs before, 0 in 6 after) and `thread_join` was 80% on one board and 0 across
twelve runs on four others. Both had been written down as marginal and left alone, and that is what
kept a genuine scheduler race hidden. **Measure a rate on both sides of a change before applying a
flake label.**

### What the captures do NOT witness

**The M4.7.6/M4.7.7, M4.7.8 and M4.8.1 silicon debts are PAID**, and those three capture sessions
are archived at `docs/archive/M4.7-M4.8.1_fleet_selftest_meas.md`: six boards, four ISAs, every
enforcement class the fleet has, and `picopi`'s first clean armv6m enforcement run. Go there for a
row. The lessons those passes taught that apply to the NEXT capture are below.

**M4.8.2 is witnessed on SIX boards at its own close** (banner `b77a3ef4`, a DEAD hash -- see
*History*), which is every enforcement class the fleet can
currently reach: `picopi` is the only gap and it is not on any bus. A scheduler change is shipped
kernel code on every board, which is why the whole fleet ran rather than one representative. Logs
`.session/logs/m482-*.log`, and **all seven streams were piped through `tests/integration/check_tap_stream.sh`
by hand** rather than read off the printed counts. `f302nucleo` is the exception that proves the rule
and its permission sets are NOT declared anywhere in the tree: they were taken from
`CONTEXT.local.md`'s provisioning list, so for that board alone the check is only as good as that
list. `microbit` is the only board whose skip set the tree states.

| board | class | plan | result |
| --- | --- | --- | --- |
| `frdmk64f` | SYSMPU | `1..95` | 95 ok, enforce |
| `esp32c6-wroom` | PMP NAPOT | `1..95` | 95 ok, enforce |
| `xmc4800-relax` | PMSAv7 | `1..95` | 95 ok, enforce |
| `rx72m` | RX MPU, and the fleet's only board with no CI gate at all | `1..95` | 95 ok, enforce |
| `esp32-wroom` | LX6, no unit, and an IMMEDIATE-switch port | `1..91` | 91 ok |
| `f302nucleo` | ring-only | `1..51` + `1..40` | 51 + 40 ok, 3 + 7 skip, 0 + 4 partial |

**M4.8.2 AND M4.8.3 are together witnessed on SEVEN boards at the M4.8.3 close**, TAG `m483` and, for the
board that came back a day late, `m483c6`. That is EVERY enforcement class the fleet has:
`esp32c6-wroom` was on no bus for the first pass and was captured at the same tree afterwards, so PMP
NAPOT and `rv32imac` are owed nothing for either milestone. `picopi` closes the PMSAv6 hole the
M4.8.2 pass left open. All ELEVEN streams were piped through
`tests/integration/check_tap_stream.sh` by hand, against arm counts derived from
`user/apps/common/selftest/CMakeLists.txt` -- 99 enforcing, 95 no-MPU, split 51 + 44 -- and never
from a capture's own plan line. Every stream returned PASS.

| board | class | service list | plan | result |
| --- | --- | --- | --- | --- |
| `frdmk64f` | SYSMPU | `kickos_services_frdmk64f` (polled `k64uart` + DSPI) | `1..99` | 99 ok, enforce |
| `xmc4800-relax` | PMSAv7 | `kickos_services_xmc4800relax` | `1..99` | 99 ok, enforce |
| `picopi` | PMSAv6, the only armv6m enforcement unit | `kickos_services_none` | `1..99` | 99 ok, enforce, 0 skip |
| `rx72m` | RX MPU, no CI gate at all | `none`, then `kickos_services_rx72m_uartirq` | `1..99` twice | 99 ok both, enforce |
| `esp32-wroom` | LX6, no unit | `none`, then `kickos_services_esp32_uartirq` | `1..95` twice | 95 ok both |
| `esp32c6-wroom` | PMP NAPOT, the fleet's only `rv32imac` unit | `none`, then `kickos_services_esp32c6_uartirq` | `1..99` twice | 99 ok both, enforce, 0 skip, 0 partial |
| `f302nucleo` | ring-only | `kickos_services_none`; NO provider exists for this board | `1..51` + `1..44` | 51 + 44 ok, 3 + 7 skip, 0 + 4 partial |

**`picopi` reported ZERO skips**, so it runs `mem_self_grant` -- the arm M4.8.3's task pool cost
`microbit`, and so does `esp32c6-wroom`. The THREE `_uartirq` runs are what make the driver half of
9.5 non-vacuous on the three boards whose default list is `kickos_services_none`; on the default
preset a green run there says nothing about a driver, and all three were run BOTH ways for that
reason. On the C6 the driver run is legible in the stream itself: `[c6uart] device up (IRQ TX/RX)`
and a tap route reading `stdout endpoint -> console driver`, against `kernel debug console` on the
default list. Its 99 arm NAMES are identical, in order, to `frdmk64f`'s and `picopi`'s, which is what
says no arm was lost rather than merely that a count reconciled.

**THE DEATH PATH ITSELF NEEDED A SECOND APP: the selftest never faults by design** ("the deliberate
cross-domain MPU fault is a separate binary", `user/apps/common/selftest/main.cc`), so a selftest
capture exercises the narrowed dying guard through exit/join/kill traffic only and says nothing about
the window between a fault redirect and its stub. `faultsurvive` (`KICKOS_FS_MODE=0`) is the arm that
does, and it is a fault under teardown pressure because root is parked in `join()`. Clean on FOUR
boards and three arch classes at the M4.8.3 close: `frdmk64f` and `xmc4800-relax` (armv7m, `PC` + `CFSR=0x10000`),
`picopi` (armv6m, `PC` only, no CFSR to print) and `esp32c6-wroom` (`rv32imac`, `PC` + `mcause=0x2`,
the illegal instruction) each printed
`=== THREAD FAULT === thread 'faulter' killed, system continues` and then
`[fs] survivor ran after the fault`, in that order, with no panic dump. That ordering is the witness:
root's line cannot precede the kill. It is also where the 9.5 banner rename shows on silicon -- the
record says `thread`, not `task`. **The C6 run is the FIRST time `kickos_fault_kill_thread` has
executed on `rv32imac` silicon** (`arch/riscv/rv32imac/arch_rv32imac.cc`; `nm` confirms it plus
`arch_fault_is_user_thread`/`arch_fault_redirect_to_exit` in the flashed image), and it was taken
under `kickos_services_none` for the swallow reason SINCE FIXED below rather than on the default list.
A `m484` capture no longer needs that dodge.

**THE SWALLOWED FAULT RECORD IS FIXED, AND THE FIX IS A ROUTE RATHER THAN A RECLAIM (M4.8.3).**
The five record sites call `kprintf_fault`, which while the console is `USER_OWNED`
also hands the line to the published endpoint's ALREADY-PARKED receiver (`cap_console_deliver`), so
the DRIVER prints it and a healthy driver keeps its device. The ruling, the four rejected options and
the two things the fix still loses are `docs/design-m4.7.9-fault-isolation.md` section 9.5; the
transport is now part of `fault-record-is-printed-only-by-its-owner`.

**Witnessed at TAG `m484`, and MUTATION-PROVEN ON SILICON on two boards.** Reverting the five call
sites to plain `kprintf` and reflashing loses the record while both `[fs]` lines and the survival
stay: `frdmk64f` under its DEFAULT polled list (`m484mut-frdmk64f-faultsurvive.log`, banner
`58e7174e-dirty`, which is the mutant labelling itself) and `esp32c6-wroom` under
`kickos_services_esp32c6_uartirq` (`m484mut-esp32c6-wroom-faultsurvive.log`). The second one is the
witness that matters: that driver is IRQ-driven and buffered, and it is exactly the case a SCOPED
reclaim would have wedged, since every `arch_console_reclaim` body clears a TX interrupt enable.

**A CAPTURE'S CRLF TELLS YOU WHICH TRANSPORT CARRIED A LINE, BUT ONLY ON A POLLED DRIVER.** The
kernel chip path cooks `\n` to CRLF where the board asks for it, and the POLLED console drivers
(`k64uart`, `xmcuart`) do not, so on `frdmk64f` and `xmc4800-relax` the driver-routed record is
visibly UNCOOKED in the same stream as the CRLF-cooked kernel banner -- a per-line transport witness
inside one capture, and the reason those two captures need no second run to interpret. **It does NOT
generalise**: `uart_service.h` (the substrate every `_uartirq` driver shares) cooks CRLF itself
(`cook_crlf`), so on `esp32c6-wroom` and `rx72m` driver-routed output is cooked exactly like the
kernel's and the tell says nothing. Reading it as "the console was never published" is wrong, and it
was read that way once in this session before the mutation settled it; the app-side witness there is
the selftest's own `# tap route: stdout endpoint -> console driver` line, and the real witness is the
mutation.

**THE ESCALATION HALF IS NOW WITNESSED ON FOUR BOARDS, having been `rxv3`-only.** `faultsurvive_ovf`
and `faultsurvive_off` must reach the PANIC dump rather than the thread kill, and until `m484` only
`rx72m` had ever run them. All fourteen `m484` captures are `commit 58e7174e`, clean, with the two
deliberate mutants stamped `-dirty`. The rx72m pair also RETIRES the `-dirty` caveat on the only
prior escalation witnesses: both reproduced byte-for-byte at a clean tree (`MPDEA=0x121fc`,
`PC=0xffc0050d PSW=0x130001`).

| board | class | app | list | verdict |
| --- | --- | --- | --- | --- |
| `frdmk64f` | SYSMPU | `faultsurvive` | `kickos_services_frdmk64f` | record ON THE WIRE, uncooked, before the survivor line |
| `frdmk64f` | SYSMPU | `faultsurvive_ovf` | default | escalates, `=== HARD FAULT ===`, `CFSR=0x1400` + `SYSMPU ISOLATION FAULT port=3 addr=0x20018b30 W` |
| `frdmk64f` | SYSMPU | `faultsurvive_off` | default | escalates, `=== HARD FAULT ===`, `CFSR=0x10000`, no stacking-abort bit |
| `frdmk64f` | SYSMPU | `selftest` | default | `1..99`, 99 ok, enforce |
| `xmc4800-relax` | PMSAv7 | `faultsurvive` | `kickos_services_xmc4800relax` | record ON THE WIRE, uncooked, before the survivor line |
| `xmc4800-relax` | PMSAv7 | `faultsurvive_ovf` | default | escalates, `=== MPU FAULT ===`, `CFSR=0x92` (MSTKERR set), `MMFAR=0x20013f78` |
| `xmc4800-relax` | PMSAv7 | `faultsurvive_off` | default | escalates, `=== HARD FAULT ===`, `CFSR=0x10000`, no stacking-abort bit |
| `xmc4800-relax` | PMSAv7 | `selftest` | default | `1..99`, 99 ok, enforce |
| `esp32c6-wroom` | PMP NAPOT | `faultsurvive` | `kickos_services_esp32c6_uartirq` | record ON THE WIRE over an IRQ-driven driver, proven by mutation |
| `esp32c6-wroom` | PMP NAPOT | `faultsurvive_ovf` | default | escalates, `MPU FAULT: thread 'faulter' attempted write at 0x40833f00` |
| `esp32c6-wroom` | PMP NAPOT | `faultsurvive_off` | default | escalates, `=== RISC-V TRAP (illegal instruction) ===`, `mstatus=0x80` (MPP=U) |
| `esp32c6-wroom` | PMP NAPOT | `selftest` | default, then `_uartirq` | `1..99` twice, 99 ok, enforce |
| `rx72m` | RX MPU | `faultsurvive` | `kickos_services_rx72m_uartirq` | record ON THE WIRE over an IRQ-driven driver |
| `rx72m` | RX MPU | `faultsurvive_ovf` | default | escalates, `MPU FAULT: thread 'faulter' attempted write at 0x121fc` |
| `rx72m` | RX MPU | `faultsurvive_off` | default | escalates, `=== RX EXCEPTION (privileged instruction) ===`, `PSW=0x130001` (PM=1, user) |
| `rx72m` | RX MPU | `selftest` | default | `1..99`, 99 ok, enforce |

**`frdmk64f`'s overflow arm latches a DIFFERENT bit from every other armv7m board, the gate
MISJUDGED it, and BOTH the misjudgement and its repair are now witnessed on that silicon.** The old
`armv7m:overflow` corroboration required `CFSR & 0x10` (MemManage MSTKERR), which is what
`xmc4800-relax` produces (`CFSR=0x92`). The K64F's SYSMPU is a BUS-level slave-port unit, not the ARM
MemManage unit, so its stacking abort latches BusFault `STKERR` (bit 12) plus `IMPRECISERR` --
`CFSR=0x1400`, `MSTKERR` CLEAR -- and the real evidence is the `SYSMPU ISOLATION FAULT` line naming
the denied write. **So the gate would have FAILED a correct capture**, and it was latent only because
no ctest runner points it at a SYSMPU board.

**TAG `m484k`, on live silicon:** `faultsurvive_ovf` gave
`=== HARD FAULT ===`, `CFSR=0x1400 HFSR=0x40000000`, and
`SYSMPU ISOLATION FAULT: port=3 addr=0x20018b30 master=0 W EDR=0x80000003` -- and the repaired gate
ACCEPTS it (`FS_CAPTURE=`, PASS on the STKERR-plus-SYSMPU shape). Until this board came back the fix
had only ever been judged against an archived log, so this is what turns the repair from argued into
witnessed.

**The same pass covers the SEGMENTED capability table, which NO host arm can reach.** `frdmk64f` runs
`KCAP_RUN_CHUNKS > 1` while the K-seam fixture only ever compiles the SIM posture, so a
flat-versus-segmented teardown difference is invisible in-env. Its `selftest` at that capture is
`1..99`, 99 ok, 0 skip, 0 partial, enforce -- with the class A `sched::wake` and task-lifecycle
fixes in the image. The count was DERIVED from `user/apps/common/selftest/CMakeLists.txt`
(81 + 14 + 3 + 1) and the stream piped through `tests/integration/check_tap_stream.sh` by hand,
never read off its own plan line.

**THE SLAY ABI IS WITNESSED ON SILICON, TAG `m484sl`, on the two boards that
matter most for it.** All five arms -- `thread_slay_window`, `thread_slay_gate`,
`thread_slay_timeout`, `task_slay_group`, `task_slay_gate` -- named and green:

| board | class | plan | why this board |
| --- | --- | --- | --- |
| `esp32-wroom` | LX6, no unit, IMMEDIATE-switch | `1..100`, 100 ok, 0 skip, 0 partial | the ONE backend whose seam claim was a reading of the code rather than a run, and the design's instruction for it was INVERTED: it said force `resume_kind` to COOP, and a fabricated frame is `KICKOS_RESUME_IRQ` -- COOP would send the switcher down the `retw` path onto an interrupt frame |
| `rx72m` | RX MPU | `1..104`, 104 ok, 0 skip, 0 partial | no emulator and no CI gate anywhere, so silicon is the only check that exists |

Both counts DERIVED from `user/apps/common/selftest/CMakeLists.txt` (86 unconditional, +14 with
the driver arms) and both streams piped through `tests/integration/check_tap_stream.sh` by hand.
**Still owed**: `picopi` (the only armv6m enforcement unit) and `frdmk64f` (the segmented cap
table, which no host arm reaches). Neither was on a bus for this pass.

**CLASS B ITEM 1 IS CLOSED ON SILICON, TAG `m484s1` in M4.8.4, and the pair is the
witness.** The guard band narrows `kickos_fault_below_stack` from "every address below the
stack base" to one RXv3 MPU region page (16 bytes) immediately below it, and the fault stub now
enters at the TOP of the dying thread's own stack on all five backends. Both bracketing captures
moved the way they had to, on the one board that can witness either:

| app | denied address | before S1 | after S1 |
| --- | --- | --- | --- |
| `mpu_fault` | `0x13200`, a cross-domain write | escalated -- the measured FALSE POSITIVE | `=== THREAD FAULT === thread 'domainA' killed, system continues` |
| `faultsurvive_ovf` | `0x121fc`, the denied push | escalated | escalates, unchanged |

`faultsurvive_ovf`'s capture also carries the new stack-range line, `its stack 0x12200-0x14200`,
so `0x121fc` is exactly `stack_base - 4` -- which is what makes the 4-byte floor on the band a
measured shape rather than a guess. No capture in the tree recorded that range before.
**The stack reset is the load-bearing half**: the damage the old test prevented was the stub
running on the exhausted stack, and a stub at the top cannot, which is what makes a band
admissible where a bare threshold was not.

**`rx72m` BASELINE at the same tip, TAG `m484rxb`**, taken because rxv3 has no emulator and no CI
gate at all, so nothing else checks it: `mpu_fault` still ESCALATES
(`MPU FAULT: thread 'domainA' attempted write at 0x13200 -- reported`). That is class B item 1's
measured false positive reproduced in M4.8.4 rather than inherited from an older archive, and it
is the before-picture the guard-band narrowing has to move.

**`picopi` IS WITNESSED, at the squashed commit itself and not at a pre-squash tip** (TAG `m483pi`,
four captures, `commit bb8fae24` on every banner). It is the fleet's only armv6m enforcement unit and
it now carries all three fault modes, where every other board's escalation capture truncates before
its dump:

| arm | frame | outcome |
| --- | --- | --- |
| `selftest` | -- | `1..99`, 99 ok, 0 skip, 0 partial, enforce |
| `faultsurvive` | `PC=0x1000029c`, no CFSR to print on armv6m | `=== THREAD FAULT === thread 'faulter' killed`, THEN `[fs] survivor ran after the fault` |
| `faultsurvive_ovf` | `PC=0xffffffff xPSR=0x0` | escalates, `=== HARD FAULT ===` |
| `faultsurvive_off` | `PC=0x100002a0 R3=0x20008200` | escalates, `=== HARD FAULT ===` |

**The two escalation frames differ for a reason worth keeping.** `_ovf` recurses off the stack, so the
hardware stacking writes into the overflowed region and the frame it reports is GARBAGE -- a
`PC` of `0xffffffff` and a zero `xPSR` are the signature, not a capture defect. `_off` moves SP
outside the stack instead, faults on the first access, and its frame is intact. An arm asserting a
plausible PC on `_ovf` would be asserting something armv6m cannot deliver.

**What armv6m still cannot witness is the RECORD ROUTE, and that is a USB defect rather than a fault
one.** `picopi`'s only publishing service is `kickos_services_picopi_usbcdc`, and publishing blinds
UART0 by design (the kernel console is a different peripheral from the one the driver takes). Captured
that way the UART carries the banner and nothing after it, and no `ttyACM` appeared within a
0.2 s-resolution poll armed before the flash. That is CONSISTENT with the already-filed blocker --
under the production service list the device reaches `[rpusb] host configured the device` and then not
one byte reaches the ACM tty -- and it does NOT establish the stronger claim that enumeration never
happened: `faultsurvive` lives a few hundred ms, and `dmesg_restrict` is 1 on this box so the kernel
log was not available to discriminate. Log `.session/logs/m483picdc-picopi-faultsurvive.log`.

**`f302nucleo`'S FAULT REPORTER WAS NEVER BROKEN -- THE FLASH COMMAND WAS.** `st-flash
--connect-under-reset --reset write` leaves the core under halting debug with `DEMCR.VC_HARDERR`
armed, so the `udf` escalates to HardFault normally and the core then enters Debug state AT
`HardFault_Handler`'s first instruction instead of executing it. CPU stopped, so no LED, no dump, and
a board that looks locked up forever. Fixed in `tools/flash-stlink.sh`: `--reset` is dropped wherever
`--connect-under-reset` is used, because releasing NRST already starts the image.

**The old reading -- "exception entry itself fails, a bad vector fetch or LOCKUP during hardware
stacking" -- is FALSIFIED, and by two independent instruments.** On the live board: `DFSR=0x9`
(`VCATCH`), `DEMCR=0x01000501`, `HFSR=0x40000000` (`FORCED` with `VECTTBL` CLEAR, so not a vector
fetch), `DHCSR` `S_LOCKUP` CLEAR at a real instruction address rather than `0xFFFFFFFE`, `CFSR=0x10000`
(`UNDEFINSTR` alone, no `STKERR`, so not a stacking fault), and a stacked frame at `PSP` carrying the
`udf`'s own PC. The single-change proof is a gdb write of `DEMCR=0x01000000` on the SAME boot with no
reset and no reflash, after which the stalled boot finished its queued line and printed its fault
report. Control: the same image under `--connect-under-reset write` with NO `--reset` reports unaided.

**It was already visible in the archive and nobody read it.**
`.session/logs/m483-fs-f302nucleo-faultsurvive.log` (2026-08-12, M4.8.3 close) holds TWO boots, and the
FIRST one prints `[fs] spawning the faulter`, `[fs] worker about to fault`, then `=== THREAD FAUL` --
the reporter running, truncated by the reset that produced the second boot. A `tail` of that file
shows only the second boot, which is how it stayed unread. **`bench-capture.sh` uses the safe order,
which is why the bench never saw the defect and only the recovery path did.**

The board still has NO MPU, so the MPU-fault arms remain genuinely out of reach there -- that part was
always a hardware fact. Fault isolation via `udf` needs none of it.

**`rxv3` NOW HAS FAULT ISOLATION AND IT IS WITNESSED ON SILICON. `lx6` still cannot have it.** The
`rte` the old decline named as never-executed has now executed, on `rx72m`, at TAG `m483rxg` -- and
the decline's reason was never wrong, it was about the TREE: there is still no RXv3 emulator and no
CI gate, so every rxv3 fault claim is unfalsifiable off that board. `lx6` is refused by
`KICKOS_HAVE_PRIV_RING` because `PS.UM` is 1 for kernel and thread alike, which is a HARDWARE fact
and not an unported feature -- it has no unprivileged thread to kill, so there is nothing for the
rule to discriminate and no backend code would change that. Do not record it as a gap of the same
kind as the Cortex-M0.

**FIVE arms on `rx72m` silicon, one tree, and the addresses are what separate them.** Every capture
is `commit f8cc32bd-dirty`, which is honest and load-bearing: `bench.sh` builds the working tree it
flashes, and the tree it flashed is the one this commit contains.

| app | fault | outcome |
| --- | --- | --- |
| `faultsurvive` (mode 0) | `mvtipl #0` in user mode, cause `0x50` | `=== THREAD FAULT === thread 'faulter' killed` at `PC=0xffc00505`, THEN `[fs] survivor ran after the fault` |
| `faultsurvive_off` (mode 2) | USP moved to `0x8200`, outside the stack | escalates, `=== RX EXCEPTION (privileged instruction) === PSW=0x130001` |
| `faultsurvive_ovf` (mode 1) | recursion off the stack, `MPDEA=0x121fc` | escalates, `MPU FAULT: thread 'faulter' attempted write` |
| `mpu_fault` | cross-domain write, `0x13200`, BELOW `domainA`'s stack | escalates, `MPU FAULT: thread 'domainA'` |
| `rxdrv` | ungranted `PORT8.PMR`, `0x8c068`, ABOVE the stack | `=== THREAD FAULT === thread 'rxdrv' killed`, `MPESTS=0x6 ADDR=0x8c068` |

**The ORDERING is the mode-0 witness, not the presence of the two lines**: root is parked in `join()`
so its line cannot precede the kill. `PC=0xffc00505` is exactly the `mvtipl #0` in `faulter`, which
also confirms the frame offset independently -- an instruction-cancelling exception saves the PC OF
the faulting instruction, so a wrong offset could not land on it.

**The mode-1 arm FAILED FIRST and that is the most useful thing in this pass.** At TAG `m483rxovf`
the overflow was KILLED, the stub ran privileged on the exhausted stack -- supervisor bypasses the RX
MPU, so nothing trapped the damage -- and it smashed its way to `=== RX EXCEPTION (trap) === PC=0x0
PSW=0x0`. Localized with `KICKOS_RX_MPU_TRACE` at TAG `m483rxovft`. Cause: RXv3 CANCELS the faulting
instruction and restores SP, so the USP reads as though the denied push never happened and no
SP-based test can see an overflow. Fixed by `kickos_fault_below_stack`, and the last two table rows
are the measured cost: `mpu_fault` used to die thread-scoped at the broken tree and now escalates,
because its target is below the faulting thread's stack. Both captures are kept.

**`f302nucleo`'s FAULT ARM IS WITNESSED AND GATE-VERIFIED, AND THE "UNRELIABLE INSTRUMENT" WAS THE
CAPTURE PROTOCOL.** TAG `m484p2`: `[fs] worker about to fault`, then
`=== THREAD FAULT === thread 'faulter' killed`, then `F3 0x8000264 CFSR 10000` (`UNDEFINSTR` alone,
the `udf` exactly), then `[fs] survivor ran after the fault` COMPLETE -- and
`check_faultsurvive.sh` accepts it (`FS_CAPTURE=`, `PASS`, with the exit clause reporting
`NOT EVALUATED` because a log carries no exit status).

The old reading -- an arm truncated at `=== THREAD FAUL` with the board restarting inside the window
-- was `.session/bench-capture.sh` issuing a separate `st-flash reset` AFTER the write. That cut the
correct boot off mid-line and started a second one that STALLED at the first `[fs]`, so every
capture came back truncated at a DIFFERENT point, which is what read as flakiness. Measured on one
image: with the reset, 2 boots and 355-412 bytes with the survivor line cut; without it, 1 boot and
300 bytes complete. A 60 s window still gave exactly 2 boots, so the second stalls rather than
loops. **The firmware was never involved**, and the single-variable control is what says so: a
write-only capture of the PRE-fix image reproduces the complete line byte for byte.
**Releasing NRST at the end of the write already starts the image, so take this board WRITE-ONLY**;
"expect two plan lines and slice from the last one" is RETIRED for it.

**A `KICKOS_DIAG_TERSE` board's banner reads as damaged to the capture script.** `bench-capture.sh`
recovers a lone 8-hex token and labels it "banner damaged in transit", which is what `f302nucleo`'s
`c f8cc32bd` gets: the terse banner is `c`/`b`/`a`/`m`, not `commit`/`board`. The label is wrong on
that board and the capture is fine.

**THE `banner:` LINE `bench-capture.sh` PRINTS STRIPS A `-dirty` SUFFIX**, so its summary cannot tell
a clean tree from a dirty one and only the log can (`grep -a 'commit ' <log>`). The three `m483c6`
images say `f8cc32bd-dirty` because a concurrent branch held uncommitted RX and doc edits, one of them
in `faultsurvive/main.cc` itself. The C6 witness holds at that capture anyway, and the two checks that
say so are cheap: the generated `.config` is identical to a pristine worktree's, and the PREPROCESSED
`rv32imac` TU of `faultsurvive/main.cc` is byte-identical (`-E`, `#` lines dropped) because every RX
delta sits behind `#elif defined(__RX__)`. Comparing OBJECTS instead proves nothing here -- `-g`
embeds the build path, so all 71 differ -- and a normalised `objdump -d` is no better on this pair,
because a `-dirty` commit string is longer and shifts every `.rodata` address after it by 4.

**The witness survived the review fixes that followed it.** The primary evidence is the source diff:
those fixes are comment-only, which `git diff` shows outright. The artifact cross-check is worth
knowing for its traps. Every kernel object AND the `text` size changed, which looks alarming and is
not: `-g` embeds the build path in DWARF, and a worktree path is longer than the main checkout's;
the `text` delta is `tests/tap/tap.h`'s `__FILE__`, which is APP-side. Comparing normalised
`objdump -d` instead, with an untouched file as the control, every instruction stream is identical.
**Two limits on that method.** It disassembles no data, so a changed `.rodata` initialiser passes
silently. And it was run on `frdmk64f`, where `KICKOS_ASSERT` expands to a stringified condition; on
a `KICKOS_DIAG_TERSE` board (`f302nucleo` is the only one) the same macro emits `__LINE__` as an
immediate, so inserting a comment line above an assert legitimately moves a constant there and must
not be read as a code change. An object-size delta alone never means the code moved.

**`f302nucleo`'s second image needed the log SPLIT before the checker would accept it.** Both its
captures contain TWO plan lines, a board restart inside the capture window that
`.session/bench-capture.sh` flags itself, and p2's whole-file stream reconciles as "plan claims 40
but 44 were reported". Feeding the last run alone passes. p2's truncated fragment had reached 4 arms
with zero failures and p1's had reached none, which is what says restart rather than fault: check the
fragment before believing a reconciliation failure on this board. **It recurred at the M4.8.3 close**, where
BOTH fragments reached zero arms, so the restart belongs to the board rather than to one image or one
tree: expect two plan lines here and slice from the last one.

**A DRIVER IS ONLY IN THE IMAGE IF THE SERVICE LIST PUTS IT THERE.** `rx72m-st`, `esp32-wroom-st` and
`esp32c6-wroom-st` all default to `kickos_services_none`, so a green run on a default preset says
NOTHING about that board's driver. This was got wrong twice in one session. `bench.sh` takes
`SERVICE_LIST=` for exactly this, and the IRQ UART services live only in the `*_uartirq` providers and
are never a default. The polled default lists also claim no IRQ line at all, which is why a timing arm
can pass there and fail under `_uartirq` on the same board.

**`f302nucleo` is the one board whose capture is not self-validating**, and its skips are
provisioning (a 3-thread pool, a 7-slot cap table), not defects. `bench.sh` prints counts but does
not run `tests/integration/check_tap_stream.sh`, so both images were piped through it by hand against the arm
counts derived from `user/apps/common/selftest/CMakeLists.txt` rather than from their own plan
lines. Do the same for any future silicon capture on a board with no ctest gate: a plan that
reconciles with itself proves nothing about an arm that was deleted.

**`.session/bench-fleet.sh` is the instrument for a fleet pass now**, and it exists because a
caller must never pair a board with a probe serial by hand. `for b in "board sn"; do bench.sh $b`
is correct in bash and silently wrong in zsh, which does not word-split: the pair arrives as one
board name and `bench.sh` dies at its configure line before printing anything, so the board reads
as skipped rather than failed. The fleet script resolves serials itself from sysfs by `idProduct`,
reports an absent board as absent, and handles the two-image boards.

**A batch `ctest` across all suites is not a valid instrument for `sim` and `qemu`.** They have no
silicon clock, fail under the load of a back-to-back run, and pass standalone. CI runs one board
per job for that reason, and so must any local sweep. **That constraint is now MECHANICAL rather
than remembered**: every gate that executes no KickOS image carries `LABELS host`, so `ctest -L host`
is the batchable set and `ctest -LE host` is exactly the set that must run standalone. The label is
defined once in the root `CMakeLists.txt`. It is not a synonym for "runs on the build host":
`oot_export` runs the app it built and deliberately does not carry it.

**`EXPECT_SKIPS` and `EXPECT_PARTIALS` are PERMISSION SETS, not budgets.**
`tests/integration/check_tap_stream.sh` fails an UNLISTED skip but only NOTEs a listed arm that did not skip.
A LOSS of arena slack is therefore caught automatically and a GAIN is not: any change that moves
`microbit`'s `.bss` needs its skip set diffed by eye.

**A silicon selftest run is `--preset <board>-st`.** `KICKOS_ENABLE_SELFTEST` is ON only on the
`-st` variants and on the boards whose base variant is itself a run gate. Getting the variant
wrong costs arms and still reads as a clean pass. Flash, WAIT for the flash script's own `r;g` to
finish, arm exactly ONE reader by its `by-id` symlink, then reset separately;
`.session/bench.sh <board> [sn]` with `TAG=<milestone>` encodes the whole order and refuses
rather than producing a plausible-looking wrong log.

## What is next (locked order)

**M5.2.1 IS THE LIVE TRACK** and is described at the top of this file. What follows is the
order after it. **This section used to carry 312 lines of M4.8.x and M4.9.x history and to
open by declaring M4.9.2 live, which contradicted the top of this same file.** That history is
in git and in `docs/design-m4.8.2-host-unit-tests.md`, `docs/design-task-layer.md` and
`docs/design-kill-and-slay.md`; it does not belong in a re-grounding note. Only the DEBTS it
carried are kept below, because a command cannot re-derive those.

**THE ORDER IS FIXED, 2026-08-21, and it reverses two earlier plans.** The user's reasoning,
verbatim: *"we need a proper foundation and driver era is just improving support, but I would rather
get a good and 'finished' kernel before"*. So: **M5.2.1** escalation + TLS, **M6** the MMU (unicore
A53 on QEMU `virt`), **M7** multicore (SMP/AMP), **M8** IPC/IRQ optimisation, **M9** back to the
driver era. `roadmap.md` is the authority and carries the scope of each.

What reversed: **the MMU now PRECEDES multicore** (it was M6=SMP, M7=MMU), because a first
A-profile port and a first SMP port are too many firsts in one bite; and **optimisation now FOLLOWS
multicore**, because a fastpath tuned before the exclusion contract exists is shaped for one core
and then reshaped for the lock. Note the numbers M6 and M7 SWAPPED MEANING: a doc saying "at M7"
about page tables means M6 now.

**M5.2.1 absorbed the old M5.2.2 and M5.2.3.** One milestone, one review, because the trusted-stack
half DELETES the red zones and panic-tail exclusions the guard half would otherwise ship and have
reviewed. Its two decision gates and the per-board tight-board rule are in `roadmap.md`; the
trap-stack analysis and the four-backend inventory are in `m5.2.1-trusted-context-plan.md`.

**THE COMMIT TARGET IS THE AUDIT'S NINE-PR PLAN**, from `kickos-codebase-audit.canvas.tsx`
`planRows`, written out in the plan doc. Nine commits each independently green, so `git bisect`
means something. **PR 1 is containment and it carries the ARMv6-M extent guard** -- that supersedes
the 2026-08-21 decision to defer it, which was taken believing the rework followed immediately
rather than spanning nine PRs. The work exists: `m5.2.1-armv6m-partial.patch` and
`m5.2.1-armv6m-trap-stack-header.h`. Stage `close` deletes it again, by design.

**TLS AND PER-THREAD KERNEL STACKS ARE THE SAME SHAPE OF PROBLEM**, which is why they share
M5.2.1. Both are per-thread storage needing a per-arch seam and costing memory the small boards do
not have. TLS is not optional: `TODO.md`'s per-thread-libc-state item is the one mechanism that
fixes `errno`, newlib reent, `thread_local` AND `malloc` together, and it REFUSES a `_REENT`-swap
hack because that leaves `thread_local` broken, so there is no cheap half-measure. Its thread
pointer is `TPIDRURW` on ARM, `tp` on RISC-V, `THREADPTR` on Xtensa, and **RX has no TLS register at
all**, so that backend needs a software-tp spike on the one arch with no emulator and no CI.

**What M7 inherits, and neither figure is a guess.** The locked fraction is 53 percent with a 43
percent leaf floor, which Amdahl-bounds a two-core big lock at **1.31x** and caps it at 1.40x, so
per-core run queues are where the payoff is rather than a later optimisation. **Do not judge M7 by
its speedup**: the hold-shortening that moves those numbers is M8, so re-derive both after M8
rather than freezing a verdict. The single `g_rv_trap_stack` becoming per-core is **DONE, not
owed**: it is `g_rv_trap_stack[KICKOS_NUM_CORES][KICKOS_RV_TRAP_STACK_SIZE]` indexed by
`arch_cpu_id()`, which folds to `0u` by preprocessor at one core, and `check_cpu_id_fold.sh` gates
the fold. So M7 substitutes rather than changing four backends. A page granule at M6
removes the pow2 tax.

**"The last big reshape" is a HOPE, not a plan** (user, 2026-08-20). If the design needs reshaping
after M7, it gets reshaped until the user is satisfied. Do not decline a reshape M8 turns out to
need on the grounds that an earlier milestone was supposed to be the last.

**M8 carries the old M5.2.4 IPC plan**, now `m8-ipc-plan.md`. Its estimates are NOT planning inputs
yet: trusted-stack transitions and then page tables both change the FIXED TERM every percentage in
it is measured against, so `CALL_MINT`'s 290/109 split and the ranking between reply-recv fusion,
the reserved slot and lock work all have to be re-taken first. `m6-fastpath-retake` lands at the
head of it, **master being unable to build an uninstrumented bench image at all**. Protection is
not assumed cheap there either: `MPU_APPLY` is 443 cycles per switch on `esp32c6-wroom`, 886 of a
3651-cycle locked round trip, and removing it is priced at `f = 0.457`, 1.37x. A canvas claiming
about 94 cycles post-precompute misread the adjacent `CALL_COPY` cell; there is no such
measurement.

**The SPI class work** (items 1 and 3 of `deferred-after-pr-train.md`, the validation hoist and the
nine divergences) is orthogonal to all of the above and can ride any milestone. It belongs with M9
but must not block the architecture work.

Consequence for `heap_bump` (`user/src/newlib_sbrk.cc`): it is written down rather than fixed,
and the reason is in the file. A kernel-mediated brk would close that one race and **would NOT
make multithreaded `malloc` safe**, newlib's bins staying unprotected while `__malloc_lock` is a
no-op, so a serialized `_sbrk` would read as safety it does not provide. **M7 does its own homework on
what it inherits, so nothing here pre-chews it**; one thing worth its attention is whether the
gate corpus is buying what it costs, M5.2.1 having added roughly ten lines of gate per line of
behaviour.

**M8 = the ABI freeze**, the last milestone. `KickOSConfigVersion.cmake` is `ExactVersion`
until then, because the ABI moves at SUBMILESTONE granularity and no two versions are
interchangeable; relaxing that is a freeze-time decision.

## Debts carried forward, which a command cannot re-derive

**`rr_interleave` ON `rx72m` IS PAID, 2026-08-20.** It was owed from M4.8.4 as the only
instrument that has ever shown the creator-hold / `Kernel::task_holds` class, where 3 of 3 runs
printed `rr order: AABBAB`. Re-run at `bcb1dda9` under `kickos_services_rx72m_uartirq`, THREE
times: `rr order: ABABAB` every time, `ok 11 - rr_interleave`, and the whole suite `1..105`,
105 ok, 0 skip, 0 partial, validated by hand through `check_tap_stream.sh`. The fix is no
longer merely argued.

**THE USB CDC DEBT IS STILL OPEN, and part of how it was written down is itself stale.** Bulk
OUT has never been exercised at all, and `Shared::configured` does not clear on a bare unplug
because no backend arms a disconnect or suspend source. **The claim that `teensy41`'s USB
backend is absent by construction is FALSE and has been since `faa7b843`**, which landed
`system/driver/imxrt1062/rt1062usb/` and the `kickos_services_teensy41_usbcdc` provider, so
that board is a second CDC witness route and not an exclusion. The three captures this debt
rests on predate their commits, so they want retaking: a witness is valid for a TREE.

## Build posture

The fleet including the sim builds `MinSizeRel` (`-Os`, `-g` re-added). It shipped `-O0` until
M4.5.2, roughly 2x the footprint, so **every silicon witness taken before it needs re-running**
-- on the K64F `-Os` dropped a PIT clock-gate-race write that `-O0` had masked. `-Os` is a preset
default, invisible through `find_package(KickOS)`, and the floors in `docs/reference/porting.md`
assume it. The one commit to revert when bisecting a footprint or timing regression is
`build: optimise the fleet (MinSizeRel)`.

The build identity is **`0.4.7`**, in both `project()` and `KICKOS_VERSION` (root `CMakeLists.txt`).
The scheme is `0.<milestone>.<submilestone>` and **the bump belongs to the milestone**: it sat at
`0.4.5-2` across all of M4.6, so every M4.6 banner shipped a submilestone behind.

## Gates

**Do not carry a tally forward from this file.** One `panicgate` case or `ringpriv` registration
moves several at once, so the number here rots every milestone and has misled repeatedly. Re-derive:

    source .session/env.sh          # MANDATORY: all four cross families need it
    cmake --preset <tree> -B <dir>
    cmake --build <dir> -j8 && ctest --test-dir <dir>

**The posture is part of the preset**, so there is no flag to pass and no cached value to
reset: a board that can enforce does so on its base variant and carries a `<board>-flat`
preset for the ring-only one. `sim` has no flat variant at all -- it enforces through host
mprotect, so enforcement is the only posture it has.

**Swept 2026-08-04 at tree `e74933d`: zero failures, every tree, both postures, and all 26 presets
configure and build.** That sweep is the measurement; the shape worth remembering is that the
enforcing and ring-only postures differ by a handful of arms and that `microbit` is the only board
in the fleet with skips.

Two facts a re-derive will not tell you:

- **`oot_export_mcu` fails deterministically when `ctest` runs without `.session/env.sh` sourced** --
  it falls through to the picolibc C-only `/usr/bin` twin. 100% reproducible without the env, 100%
  absent with it. It is an environment artifact, not a code failure, and it re-configures in a
  subprocess so the toolchain vars must be exported at ctest time too.
- **`qemu-riscv` under enforcement is the only posture reporting zero partials** where every ARM
  enforcing posture reports one. Both are "all expected"; it is an encoded per-arch difference, not a
  defect, and it reads as a bug later if nobody says so.
- **`-L host` and `-LE host` do NOT sum to the sim total, and the partition is fine.** ctest pulls a
  required fixture into every filtered selection, so `kickos_build` appears in BOTH lists. **Do not
  quote a split from here either: `test_labels` PRINTS it** (`N test(s): H host, I image, 1 build
  fixture`), which is the point of the gate. The `host` label is load-bearing --
  `kickos_decline_image_tests` disables anything unlabelled when
  `KICKOS_BUILD_INTEGRATION_TESTS=OFF` -- and BOTH directions are now gated: a host test missing
  the label, and an image test wrongly carrying it. Classification is DECLARED in
  `tests/static/test_classes.txt`, because no sound derived discriminator exists --
  `check_oot_export.sh` and `check_oot_export_mcu.sh` sit in one directory with one argument shape
  and opposite classes, so an undeclared program is REFUSED rather than guessed.
- **The other direction of that gate runs on ONE tree, by design.** A declaration that no board runs
  any more is dead, and CMake passes `<complete> yes` only where the tree registers the whole
  build-rooted set -- the sim arch with the host unit binaries, so ONLY a sim run with
  `-DCMAKE_PREFIX_PATH` pointing at a GTest checks it. A fleet pass that skips that configuration
  checks the `build` half nowhere, and no board reports a skip saying so.
- **A per-test `cmake_language(DEFER CALL f "${var}")` expands its argument WHEN THE CALL RUNS, not
  when it is issued.** So every per-directory defer received the last name `add_test` had set there
  and only ONE image test per directory was ever declined: broken since M4.8.2, and nothing noticed
  because no CI job uses that knob. `kickos_decline_image_tests` now accumulates names on a
  DIRECTORY property and reads them once. The declined sim tree went from 14 disabled to 22.

**RE-MEASURED 2026-08-15, because a delegated run hit it twice and could not name it.** Batched
`ctest -j8` over the whole `sim-telem` suite fails **2 of 6 under CPU load** and **0 of 3 unloaded**;
standalone it has never failed. The failures are always IMAGE tests -- `selftest` and
`sim_published_console` -- i.e. exactly the `-LE host` set this file already says must run
standalone. So this is the documented sensitivity reproducing, not a new defect and not a flake:
the instrument is invalid, and `test_classes.txt` is what names which tests it is invalid for.

**The sim has no virtual time and its gates are not deterministic**: `arch_clock_now` reads
`CLOCK_MONOTONIC` and the tickless one-shot is a real `timer_create` delivering SIGALRM, so
preemption lands at an arbitrary host instruction and a sim gate can fail load-dependently.

`doc_names` catches a cross-reference that no longer resolves and has already caught two regressions,
one of them 33 references reverted by a rebase. Run it after any doc-heavy rebase; a clean
`git rebase` is not evidence. **It reads TRACKED MARKDOWN ONLY**, so the same citations in source
comments, CMake strings and workflow YAML rot silently -- and it validates a PATH and an IDENTIFIER,
never a LINE NUMBER, which is why this tree cites path + symbol and `design-capability-table.md` now
carries no `path:line` at all.
**Its identifier ORACLE excluded nothing under `docs/`, and the M4.5.1 audit ledger tracked there
was an .html -- TRACKED and not markdown**, so every name that file mentioned stayed valid forever:
39 markdown references to two deliberately-deleted knobs resolved against a dated snapshot. `docs/`
is now out of the oracle, not out of the checked corpus, and the ledger itself has since been
deleted. **The exclusion is the fix and it OUTLIVES that file**: the next non-markdown document
committed under `docs/` would reopen the hole. **Widening it further was measured and
REFUSED**: of the path half, 544 citations resolve, 269 are file-relative and 51 are unexpanded
shell or CMake variables, so real breakages come out at roughly 3% precision -- and the gate's own
header lives by "a checker that cries wolf gets disabled". What this class actually needs is the
habit: read every doc the milestone's diff touched and ask whether anything now says two things.

**Four more gates were not gating, and each failed silently rather than loudly.** A gate is an
artifact that can be deleted, which is why this tree mutation-tests them:
- `check_faultsurvive.sh` covered two of five enforcement classes and would have FAILED a correct
  `frdmk64f` capture: it demanded MemManage `MSTKERR`, and a BUS-level SYSMPU latches BusFault
  `STKERR` with `MSTKERR` clear. It now takes `armv6m` and `rxv3` too, and `FS_CAPTURE=<log>` judges
  a captured stream, because those two classes have NO runner to boot on -- their clauses would
  otherwise have shipped never once executed. A log carries no exit status, so the exit clauses
  report `NOT EVALUATED` rather than passing on an absent value.
- `tests/lib/panic.ere` never matched `=== RX EXCEPTION` or `=== XTENSA EXCEPTION`, so every ctest
  `FAIL_REGULAR_EXPRESSION` and every `assert_no_panic` was blind to the rxv3 and lx6 reporters --
  exactly the "a system that panicked after printing both lines would look the same" case
  `check_faultsurvive.sh`'s own header warns about.
- **Three gates parsed tab-separated records with `IFS=$'\t'`, which is a bashism, and `/bin/sh` IS
  dash here as well as on CI.** dash sets `IFS` to the three characters `$ \ t`, so records split on
  the letter `t`: a path came back as `/var/` and `mp/libkickos_kernel.a`. `check_seam_defaults.sh`
  leg 1 tests `case "$arch" in *libkickos_kernel.a)`, which a truncated value can never match, so
  that leg was VACUOUS. `dash -n` passes the bashism, so nothing catches it. `gate.sh` owns `TAB`.
- `extern_c_linkage` is new and registered on every board. `extern "C"` OVERRIDES a nested anonymous
  namespace and emits unmangled GLOBALS into libkickos's public C surface; `static` is what survives
  it. 19 symbols across four backends, and the seventh site was found by an `nm` CLASS sweep that a
  construct scan cannot see. The gate refuses a file it cannot count rather than reading it clean.

**`check_c_headers.sh` COMPILES WITH NO `-D` AT ALL, so a C-facing header is only ever checked in
its DEFAULT branch.** Found in M5 while adding a Kconfig-driven seam. Any C-facing header that
grows a `#if KICKOS_<knob>` has its other arm compiled by nothing, on any board, and the gate still
reports PASS over its full corpus. Not triggered yet: the seam that found it lives in
`arch/include/kickos/arch/arch.h`, which is deliberately C++-only and correctly outside that
corpus. **Not fixed on purpose** -- widening it means compiling every header under a matrix of knob
values, which wants measuring before it is built, the same way the doc gate's path half was
measured at roughly 3 percent precision and REFUSED.

**The selftest arm count is an EXACT FLOOR per posture** in `user/apps/common/selftest/CMakeLists.txt`
(`_tap_arms`, plus the independent partition `_tap_arms_p1` + `_tap_arms_p2` for the two-image split,
with a totality FATAL if they disagree), so adding a case without moving both fails loudly rather
than passing quietly. **Adding a `static` to a shared test is not free**: it comes out of `microbit`'s
16 KiB arena, which is why a new case reports over its own endpoint rather than through file-scope
state. `-KOS_ENOMEM` cannot distinguish a full cap table from an empty pool, which is why
`-KOS_EMFILE` exists.

**`ringpriv` and `ringppb` are permanent CI, not bench captures**: `cmake --preset qemu` IS the
ring-only posture. `microbit` asserts the OPPOSITE outcome with one arm (`CONTROL.nPRIV == 0`) rather
than skipping, machine-checking the armv6m classification, and does not build `ringppb` -- on a
no-ring core the PPB read legitimately succeeds.

## Board matrix

**Every board runs an unprivileged root by construction**, so a row records which ENFORCEMENT
backend it can witness with: **enforcing** (a ring AND an MPU/PMP backend -- the only class that
can witness memory confinement), **ring-only** (a real ring, no MPU backend), **no-ring** (no
privilege axis, but the authority word is software and still bites, which is why `microbit` runs
`rootauth`).

- **enforcing** (14): `xmc4800-relax` (PMSAv7, the flagship and the only board carrying the
  class-2 write seam), `frdmk64f` (SYSMPU), `pizero2350` (PMSAv8), `f411disco` + `blackpill`
  (shared PMSAv7), `teensy41` (PMSAv7 + ERR011573), `picopi` (PMSAv6, the fleet's only armv6m
  enforcement proof), `rx72m` (RX MPU, no CI gate), `esp32c6-wroom` (PMP NAPOT), and the five run
  gates `qemu` / `qemu-m3` / `qemu-m7` / `qemu-m33` / `qemu-riscv`.
- **ring-only** (3): `f302nucleo` -- the only physically present no-MPU ARM board, and the ring
  arm's silicon witness; `bluepill-c8` (no unit, build-only); `due` (unit retired).
- **no-ring** (3): `microbit`, `esp32-wroom`, `sim`.

Per-board chips, cores and the fact that decides each class: `docs/reference/boards.md`.

**WHAT M4.8.4 LEFT UNCOVERED, carried past the merge.** None of it is a defect and none of it blocks
anything; it is the list of things nothing currently measures.
- **THIS WAS BACKWARDS THE WHOLE TIME: the host compiles the SEGMENTED path, and it is the FLAT
  path that reaches no host arm.** Derive it, do not read it here: the sim's
  generated `config/cap_width.h` carries `KICKOS_MAX_HANDLES 10` against `KCAP_CHUNK_TARGET 8`, so
  `cap.h`'s `KICKOS_MAX_HANDLES <= KCAP_CHUNK_TARGET` is FALSE and the host takes the `#else` arm at
  `KCAP_RUN_CHUNKS == 2`. So `KCAP_RUN_CHUNKS > 1` is what every host arm has been exercising all
  along, and `frdmk64f` was never the only instrument for it.
  **What genuinely reaches no host arm is the FLAT path**, which belongs to the 7-handle boards:
  `microbit`, `bluepill-c8` and `f302nucleo`. Two of those three have no runner at all and the
  third cannot produce a clean witness, so the uncovered side is the harder one to reach, not the
  easier. Note the two paths differ where it matters: `cap_reply_live` SCANS the run on the flat
  path and reads a stored counter on the segmented one.
  It was load-bearing: it is why `frdmk64f` losing its route to silicon read as losing a coverage
  class.
- **The SYSMPU enforcement class has an instrument only while someone is present**, for the same
  reason, and `m492k` is one.
- **`picopi` owes a slay capture.** It is the fleet's only armv6m enforcement unit, and it was on no
  bus for the `m484sl` pass. `esp32-wroom` (lx6) and `rx72m` (rxv3) were captured there because they
  are the two backends with no emulator and no CI gate at all.
- **The `<complete>` flag's silent direction.** If no run in a pass claims it, the `build` half of
  `test_classes.txt` is checked NOWHERE and nothing reports a skip -- a CI matrix that drops the
  GTest-bearing sim job loses that check without a red.
- **`ctest -LE host` has STILL never been swept.** `tools/sweep_host_gates.sh` covers `-L host`
  over all 51 presets, and did so green at the M4.9.2 tree, but it says nothing about the other
  half BY CONSTRUCTION, because that half must run standalone (see *Gates*). There is no tool for
  it yet, and M4.9.2 neither closed this nor made it worse.
- **`kickos_terminate`'s device drain has no witness**, and only three chips have a body.
- **`user/apps/common/usbcdcwit` is built by no default configuration of any board.** It is gated on
  `KICKOS_SERVICE_LIST MATCHES "_usbcdc$"`, so only an explicit `-DKICKOS_SERVICE_LIST` reaches it --
  and that is the M4.9.1 witness app.
- **`f302nucleo` cannot produce a clean witness.** Its VCOM drops bytes, the banner arrives damaged,
  and the recovery path now reports `-UNVERIFIED` rather than inventing a clean hash -- correctly,
  since damage is byte LOSS and an absent `-dirty` suffix is indistinguishable from an eaten one. So
  every capture from this board reads UNVERIFIED until the byte loss itself is fixed. The `m485sq`
  logs do carry `c 3e35aaee` in the stream; it is the SUMMARY that cannot vouch for it.

**AND THE INSTRUMENT LESSON THIS MILESTONE KEEPS REPEATING**, because it cost real time three
separate times: a configuration nothing routinely runs is one nothing routinely checks. The K-seam
had not linked under `sim-telem` since M4.8.2. The `rr_interleave` regression showed only under the
`_uartirq` list, on the one board with no emulator and no CI gate, so the default-list fleet pass
could never have caught it. And the third: an alternative service list is never LINKED by anything
routine, so `user/apps/common/usbcdcwit` -- gated on `KICKOS_SERVICE_LIST MATCHES "_usbcdc$"` --
is built by no default configuration of any board, on a branch whose whole subject is USB CDC.

An earlier draft of this paragraph said `rxsci.cc` produced no object in a ten-board fleet build.
THAT WAS FALSE and is corrected below: it is compiled by the default build. The false version is
recorded here rather than deleted because it was argued from three boards defaulting to
`kickos_services_none`, which is true and still does not imply the conclusion.

**TWO OF THOSE THREE NOW HAVE A MECHANISM, AND THE THIRD IS THE BENCH'S.**
`service_lists` is registered on every board and pins every `kickos_services_*` provider to a
configure preset OF ITS OWN BOARD in `tests/static/service_lists.txt`, refusing an undeclared
provider the way `test_labels` refuses an unclassified program and reporting a declaration whose
provider is gone. The preset is not free choice: the board comes from the directory the provider's
own SOURCE lives in, so a file naming `sim` for all thirteen is red, not green.
`tools/sweep_host_gates.sh` is the other half and is an OPERATOR TOOL, not a gate -- it configures
all 51 visible presets and runs `ctest -L host` on each, which is hours and all four cross families.
Resumable, refuses a missing GTest prefix rather than sweeping a sim with its host arms cut out, and
appends its own `DONE` line so a truncated summary is visibly truncated:

    source .session/env.sh && tools/sweep_host_gates.sh     # SWEEP_OUT=<dir>, SWEEP_FORCE=1

**Neither is completeness, and the residue is where the next defect of this shape will be.**
COMPILED IS NOT LINKED: measured 2026-08-15 in M4.8.4, `cmake --preset rx72m` plus a default
build DOES compile `rxsci.cc` and `kickos_services_rx72m_uartirq`, because every provider and driver
under a chip's branch is an ordinary target in `all`. Eleven of the thirteen provider archives come
out of the ten-board fleet build and the other two out of `sim`, so the compile half is already
covered and the declaration only records WHERE. What nothing routinely does is LINK an alternative
provider into an image or RUN it: ten of the thirteen reach an image only under an explicit
`-DKICKOS_SERVICE_LIST`.
The sweep is `-L host` only and says nothing about the `-LE host` half by construction.

**THE SWEEP HAS BEEN RUN, 2026-08-15 in M4.8.4:** `DONE 51 preset(s): 51 pass (0 reused),
0 fail`. `sim` and `sim-telem` both register **204** host tests, which is the K-seam linking under
`sim-telem` -- the original defect, now witnessed across the fleet and not on one preset. The board
presets register 8, 9 or 10, and that spread is ACCOUNTED FOR, not tolerated: `kernel_ctor_placement`
is conditional on `armv7m` + MPU (it needs `.kickos_app_init_array`, which only an enforced armv7m
link emits) and `oot_export_mcu` on `qemu`. Checked in the inverse direction too -- zero `armv7m`+MPU
presets fail to register the ctor gate, so no preset is silently missing one it qualifies for.

**Tier 3 is the bench's half and is TRACKED TOOLING as of M4.8.4:** `tools/bench/` holds the
flash-and-capture order, every refusal, the TAP validation and the coverage derivation, while the rig
values (FTDI serials, paths, host) stay in gitignored `.session/rig.conf` and the tracked side
REFUSES BY NAME when one is absent. `.session/bench.sh` and `.session/bench-fleet.sh` survive as
symlinks, which cannot drift into a second truth the way a wrapper can. `bench-fleet.sh` derives the
service lists each board owes from the tree and ends `INCOMPLETE` with a non-zero exit when a
declared list was not run, so an absent board reads as NOT RUN rather than as a pass.

**AND THE FIRST FLEET PASS IT DROVE FOUND TWO DEFECTS, NEITHER IN THE KERNEL.**
`thread_slay_timeout` failed on xmc4800-relax under `_uartirq` only, green on that board's two
other lists. Instrumented on silicon, the three lists answered `rc=0 elapsed=44us` against
`rc=-110 elapsed=60069us` twice: the hog's 60ms starvation window runs from the hog's FIRST RUN,
not from the slay, and under an interrupt-driven console the caller loses all of it in between.
With nothing outranking the victim it died at once and 0 is the CORRECT answer -- the arm asserted
a starvation it had not established. Fixed by making the precondition explicit and RECOVERING a
spent window rather than skipping; skipping would leave the guarantee unexercised under exactly the
console that broke it. An intermediate fix skipped on all three lists by reading "not yet run" as
"spent" -- the inverse -- which is why the per-list SKIP count is checked and not just the failures.

The second is a witness-integrity bug and matters beyond its board. The f302nucleo VCOM drops bytes,
so a banner can arrive as `c <hash>`; the recovery path matched a bare 8-hex token and DROPPED the
`-dirty` suffix, so a capture from a modified tree read as a witness at the commit. Seen because one
dirty-tree pass reported a clean hash on that board while seven other captures in the same run
reported dirty. The recovery now carries the suffix, and a recovery that finds none reports
`-UNVERIFIED` rather than clean: damage is byte LOSS, so an absent suffix and an eaten one are
indistinguishable. INVISIBLE whenever the tree is clean, which is why three passes did not show it.

Tracking those files also proved the doc gate's own weakness: `bench.sh` carried a comment naming
`CONFIG_KICKOS_TIMED_WAIT`, a REMOVED knob. Untracked it was inert; tracked, it joins
`check_doc_names.sh`'s valid-identifier corpus and would have made that dead name permanently valid
in every doc. Same class as the audit HTML that masked two knobs. Fixed at the comment, AND at the
gate: `check_doc_names.sh` now STRIPS COMMENTS (type-aware, because `#` opens a comment in
sh/CMake/Kconfig and the preprocessor in C) before harvesting identifiers, so prose no longer confers
validity. Measured before choosing: only 13 identifiers were comment-only and just one was cited by a
doc -- the negated-error metasyntax, rewritten as the wildcard `KOS_E*` the gate already supports. The
alternative considered and REJECTED was deriving validity from definition sites, measured at 27
refused doc-cited names, nearly all of them env vars, shell assignments and `constexpr` members that
a definition rule cannot see without re-implementing five languages' syntax -- and every gap in such
a rule is a false refusal a later engineer fixes by loosening it.

The same measurement found a SEPARATE defect, witnessed: the identifier regex had no left word
boundary, so `grep -o` cut the CAP_-prefixed tail out of a KCAP_-prefixed name. Twenty-six names that exist
nowhere were valid as substrings, and a doc could drop the K from any `KCAP_` name and pass. Fixed
with a boundary on both sides plus `KCAP` in the alternation -- without which those names would match
on NEITHER side and go silently unchecked. Zero doc cited a phantom standalone, so the fix cost
nothing to land.

## Open blockers

- **PAID 2026-08-12 in M4.8.3: narrowing the dying guard puts MORE traffic through the preemptible
  window between a fault redirect and its stub.** The coupling runs the wrong way, so
  `fault-record-is-printed-only-by-its-owner` carried the weight and no host gate could discharge it.
  It wanted an enforcing board with a fault arm under teardown pressure, and it got four:
  `faultsurvive` mode 0 on `frdmk64f`, `xmc4800-relax`, `picopi` and `esp32c6-wroom`, root parked in
  `join()`, kill
  banner then survivor in that order, no panic. The fleet table above has the run. **What the pass
  ALSO found is that the record is invisible whenever a userspace console driver holds the UART**, so
  the invariant holds for the owner and the observer sees nothing -- proven both ways on one board.
  STILL OPEN and NOT discharged by any of this: the escalation half (`faultsurvive_ovf` /
  `faultsurvive_off`) has no silicon witness on ANY board, `rv32imac` included -- its mode-0 half is
  now witnessed there and its escalation half is not. Related and NOT
  independent:
  `endpoint_wait_timeout` already deflates a dying thread's priority from the timer, in the chunk
  gap, with no `dying` test, which lowers the very quantity the new guard compares against.

- **FIXED 2026-08-02: `ktime_rearm` re-derived the deadline it programmed.** It applied the
  min-delta floor itself against a fresh clock read, on every context switch, so a deadline inside
  the window was a DIFFERENT value each call -- exactly what every backend dedups on. The dedup
  never hit, the one-shot restarted before its compare, and such a deadline starved. The floor now
  lives where the deadline is BORN (`ktime_sleep_until`; `arm_slice` already had it) and
  `ktime_rearm` passes the absolute deadline through. Proven on all six boards (`m461n-*`) and by
  the `ktime_rearm` ctest, REGISTERED because it passes; mutation-proved by restoring the floor,
  which prints the drift directly (1017500, 1020000, 1022500 ... where 1015000 is wanted).
  **The two non-obvious boards are both green.** `esp32-wroom` needed a separate arch fix first --
  Xtensa `CCOMPARE0` is an equality match, so a compare landing behind the counter is MISSED rather
  than late and the next match is a 2^32-cycle wrap away, which presents as a hang at `sleep_order`
  (`b4e888d`). `rx72m` runs `rr_interleave` green with `rr order: ABABAB` under MPU enforcement.

- **FIXED 2026-08-02: the console reclaim was keyed on the endpoint's last RECEIVER.** `recv_holders`
  counts WAIT-bearing caps, so on a two-thread driver it hit zero when the SERVICE thread died --
  and the registers belong to the IRQ thread, which parks on a line cap and is not counted. The
  reclaim reprogrammed the UART under a live owner and silenced its source. It is now asked of the
  DEVICE: `console_on_driver_death` defers while any live domain holds `arch_console_reclaim_window()`.
  **"The driver is gone" is not expressible from kernel state, by design** -- `domain_for` skips the
  dedup loop for an MMIO grant and the dedup loop requires one region, so a driver's threads sit in
  different `Domain` objects: one grant, one domain, one thread. The isolation principle is what
  made the driver invisible to the kernel, so this defect was a consequence of it, not an oversight.
  The window comes from the ARCH rather than the publisher, because asking the publisher means
  trusting an unvalidated userspace address for the kernel's own registers -- and because storing
  the pair costs 8 bytes of `.bss`, which is enough to red `microbit_selftest` (measured).
  `KOS_SYS_THREAD_KILL` is what makes the deferred reclaim terminate: cooperative cancellation,
  gated on **spawn parenthood** rather than an authority bit or a capability, since parenthood
  grants the caller nothing it did not already have and cannot be delegated. Gated by
  `sim_driver_death` case 3, mutation-proved four ways. **No silicon**: the two real drivers are
  compile-verified only.

- **`kos_print` does not survive a published console**, and that is the whole of it -- NOT "app
  output is invisible". `printf` and `std::cout` DO reach a published driver via three
  publish-aware writers kept in step (`user/include/kickos/sys/emit.h`). What drops is
  `kos_print` / `kos_kconsole_write`, and RTT still carries it. **Silence from a `kos_print`-only app
  is not evidence of a dead driver**: the write is dropped before it reaches the ring, so a working
  driver and a dead one look identical on the wire -- pick a `printf` app (`gpioblink`) to probe one.
  The remaining exposure is a freestanding app using `kos_print`. What is INHERENT and not a bug: on
  a chip where the driver takes the UART the kernel was using, the kernel must let go first, so
  kernel-console writes in that span are dropped by construction -- one device, one owner.
- **M4.6.2's CDC console validated the descriptor tables on `pizero2350`**: the host binds
  `cdc_acm`, `lsusb -v` parses both interfaces, and bulk IN carried 918 bytes byte-exactly with
  `drop=0 used=0` -- which covers the chapter 9 request machine and the multi-packet EP0 data
  stage, the parts flagged highest-risk because no USB 2.0 or CDC/PSTN specification is in the
  local reference set. **The "not one byte reaches the ACM tty under the production list" blocker
  that used to sit here is DELETED, not annotated: it was false.** `[rpusb] host configured the
  device` is not a string in the tree and the DIAG service list it named has not existed since
  well before M4.9.1 -- both were scratch instrumentation. M4.9.1 measured `picopi` delivering 8192 of
  8192 offered bytes with `drop=0`, and the "~2.7 KiB dropped at teardown" figure was a CAPTURE
  HARNESS artifact: `stty -F` opened and closed the ACM before the reader armed, and `cdc_acm`
  discards its receive buffer on last close, so the loss was at the HEAD and never happened on
  the device. STILL OPEN and unchanged: **bulk OUT has never been exercised at all**, `teensy41`
  is a marked seam rather than a half-built backend, and **`Shared::configured` does not clear on
  unplug** because no backend arms a disconnect or suspend source.
- **DISSOLVED 2026-08-16, and it was never a shutdown-path defect: `picopi` under a published CDC
  console did not return to BOOTSEL because ROOT WAS DEADLOCKED and never reached shutdown at all.**
  The selftest's `t_irq` claimed a bare IRQ line 5, which is RP2040's `USBCTRL_IRQ`, so the rpusb
  driver already owned it; `kos_irq_attach` correctly answered `-KOS_EBUSY`, the arm DISCARDED that
  return, no ISR was ever bound, `irq_waiter` parked on a semaphore nothing would post, and
  `wait_n(2)` took root down with it. Nothing was going to reboot into the bootloader.
  **The "positive tell" recorded here -- `2e8a:0003` gone, `1209:0001` PRESENT, zero further bytes
  -- is exactly what a parked root behind a HEALTHY USB driver looks like**, which is why it read as
  a shutdown defect. It reproduced on both trees because both trees had the deadlock.
  Fixed on `M4.9.1-usb-cdc`: the return is checked and the line comes from
  `KICKOS_IRQ_SOFT_ONLY_BASE + 1`. The board now runs `1..104` and returns to BOOTSEL by itself.
  **The lesson outlives the bug: a discarded syscall return turned a correct refusal into a silent
  deadlock**, where the same collision on RP2350 hit an arm that CHECKED and was found in minutes.
- **CLOSED -- `f302nucleo`'s silent fault report was the FLASH COMMAND, not the firmware**, root cause
  and evidence in *Where we are* above. The LED probe that read "dark forever, the reporter entry
  never ran" was correct and is EXPLAINED by it: the core was halted at the handler's first
  instruction, so nothing downstream of it could run. Two instrument lessons outlive the bug. The UART
  markers were CONFOUNDED -- every marker that fires runs with `CR1.TXEIE` clear, so every reading
  past that point was a false negative; use a raw `GPIOB->BSRR` LED store with cycle-counted dwells
  instead. And the `CFSR=0x00008200`/`BFAR=0xE000ED00` reading quoted as this defect's evidence for
  weeks belongs to `ringppb`, not to `fault`. **A debugger left armed is a legitimate suspect before
  the silicon is**: three hardware hypotheses were carried for weeks and all three were dead. The
  app DIRECTORY is `ringpriv/`, but the probe, the capture and `boards.md`'s row are all `ringppb`
  (`user/apps/common/ringpriv/ppb.cc`), which is why quoting the directory name misfiled it.
- **No emulated gate can exercise a buffered-ring panic flush, and the sim cannot substitute** (its
  ring is provably empty at panic time; deleting `console_tx_flush_sync()` leaves the sim suite
  green). The drain is witnessed on `pizero2350` with a measured non-empty ring (`used_at_panic=419`
  of 511, 0 after the flush) and a negative control that strands all 419. Still no automated gate:
  the in-env hole is open, the behaviour is not in doubt.
- **`microbit` HAS NO ARENA SLACK AT ALL, and that is a structural fact, not a tight budget.**
  Measured 2026-08-11: `__kickos_ram_start` IS `_ebss`, both at `0x20001AE0`, so the arena begins
  exactly where `.bss` ends. The granule is 32 bytes, so ANY non-zero `.bss` addition, even four
  bytes, moves the arena base a full granule and costs one `kos_ram_alloc` grain. Proven both ways: a
  4-byte object and a single-slot pool move it identically. **So on this board the question is never
  "how many bytes" but "any bytes at all"**, and every future `.bss` growth will keep flipping an arm
  until something structural changes. M4.8.3's task pool took `mem_self_grant`'s grain, which is now
  a declared skip; the arm still runs on 13 other boards including `picopi`, the other armv6m part.
  **This is NOT a fleet property, and reading it as one would be the wrong lesson.** It is where that
  board's arena base comes from. Check the two symbols before assuming either shape.
- **The gap between `_ebss` and the arena base is NOT slack that absorbs `.bss` growth.** An earlier
  note read `bluepill-c8`'s base as a fixed `0x20003400` against an `_ebss` of `0x200013E4` and
  concluded growth lands in the gap: wrong. That gap was the fixed-size `.userheap` carve, which
  SLIDES UP with `_ebss`, so the base tracks `.bss` granule-for-granule like everywhere else. Across
  `bluepill-c8-st`'s images the base spans 4,224 B. **The shape that really is pinned is an
  enforcement window**: on `frdmk64f +MPU` every image starts at the same address, so there one
  number per board is legitimate.
- **`KICKOS_POOL_ARENA_ASSERT` is mandatory on all 16 linker scripts** (M4.9.3), so a board can no
  longer ship thread slots its arena cannot seat. Two things worth keeping: `boards/qemu-m33/mps2.ld`
  is a BOARD-LOCAL script, so any sweep over `arch/*/chip/` alone misses a linker script; and the
  worst-image margins are thin on the small parts (`bluepill-c8` +2,560 B, `bluepill-c8-st`
  +4,096 B, `frdmk64f{,-st} +MPU` +7,072 B), so static-RAM growth in a SHARED test now breaks those
  links on the POOL assert rather than the boot one. `bluepill-c8` has no ctest gate and no unit, so
  only a full-fleet build catches it. **Neither board is silicon-witnessed.**
- **CLOSED IN M4.9.2, and this entry said FOUR until the count was re-derived from the tree:
  EIGHT chips carry `arch_console_reclaim` and the same eight carry
  `arch_console_reclaim_window`** -- `mk64f`, `xmc4800`, `esp32`, `esp32c6`, `rp2040`, `rp2350`,
  `imxrt1062` and `rx72m`, plus the sim's window. The four without either are
  `stm32f302`, `stm32f411`, `stm32f103` and `sam3x8e`, which are exactly the four
  console-publishing chips with NO userspace console driver, so a reclaim there has nothing to
  reclaim from and the debt is the DRIVER's, not the seam's.
  **Derive this count, never quote it**: the first sweep written for it missed every
  `_window` body because the pattern was anchored too tightly, and returned zero.
- **`arch_console_flush_sync` is a DEVICE drain and NINE chips have a body** (the eight above plus
  `stm32f302`), where this entry claimed two. The three still on the fallback are `stm32f411`,
  `stm32f103` and `sam3x8e`, so on those `kickos_terminate` empties the console RING and then
  stops the core with whatever is still in the UART FIFO.
  **What IS still thin is `arch_console_retune`, which has exactly TWO bodies**, `mk64f` and
  `xmc4800`: the six chips that gained a console driver during the driver era have a flush and no
  retune, so a clock change under a userspace console on any of them re-derives no baud. `arch.h` used to document the seam as a clock-retune hook
  only, which is HOW the terminal path ended up with no drain: it read as "no retune, no body
  needed". The seam now states both callers and the bound the panic path needs, `kickos_terminate`
  calls it, and `stm32f302` has a body waiting on `ISR.TC`. **NO WITNESS** -- this is argued from
  the seam, and the f302 truncation that led here turned out to be the capture protocol instead.
- **FOUR in-tree apps grant a DEV window a live board-service driver already holds**, which the
  one-holder-per-window check refuses. Silicon-only: no in-env gate covers any of them.
- **A missed `KICKOS_APP_AUTHORITY` declaration surfaces only at runtime.** The kernel cannot know
  what an app will call, so there is no configure-time equivalent of the root-MMIO `FATAL_ERROR`;
  the failure is a checked `-KOS_EPERM`. The nastiest shape is an app that ignores a failed
  `pinmux_set` and then drives an unmuxed pin.
- **`Debug` is not a supported configuration on the 64 KiB boards** (decided in M4.5.5, not a
  blocker). It is the whole class, and the overflow moves every milestone that grows the suite,
  so re-measure rather than quoting -- `docs/reference/porting.md` holds the current figures.

## History that must not be garbage-collected

**M4.9.1's SILICON WITNESSES BANNER COMMITS ITS OWN SQUASH DESTROYED**, and
`backup/m491-presquash-20260816` is what keeps them resolvable -- local and unpushed, like every
branch named in this section. `usbcdcwit` on `pizero2350` stamps `e0ab9cf9` and the `teensy41`
selftest `a4a3d8dc`; the `picopi` run carries no banner at all, its console being the device, so its
tree is named only here. **The squash changed NO CONTENT** -- the four-commit tip is byte-identical
to that backup outside this file -- so those captures do describe the code that shipped, and the
backup is what lets a reader CHECK that rather than take it.

**`c296feb` is reachable only from the local unpushed branch `m4.2-presquash`.** It holds
`git show c296feb:docs/design-m4-rx-irq-demux.md`, which `docs/design-m4.6-irq-driver.md` section 6
cites rather than reproduces for the RX routing-class taxonomy, the group-register table and the
level-versus-edge semantics. One `git branch -D m4.2-presquash` destroys an M4.6.1 prerequisite.

Captures and records across `TODO.md` and `docs/` stamp pre-squash tips (`c5d9b0d`, `270b6fa`,
`124b68c`, `989af16`, `16e4af0`, `788b1d8`) that folded into `dde73ca` and reach no branch. The
stamps stay as written.

**ELEVEN HASHES CITED ACROSS THIS FILE AND `TODO.md` SURVIVE ON THIS BOX ONLY.** Re-derived
2026-08-16, and the earlier wording -- that squashes "destroyed" them -- was wrong in the direction
that matters: every one still resolves HERE, each held by a local backup branch, and NONE of those
branches is pushed (`git ls-remote --heads origin` carries 27 heads and only the two
`backup/m4.5.2-*` among them). So a fresh clone loses all eleven, and so does one `git branch -D`.
The squashed commits carry only their final tree; every hash below is an INTERMEDIATE tree that no
current commit reproduces.

| hash | what it stamps | reachable only from |
| --- | --- | --- |
| `b77a3ef4`, `a2695e08` | M4.8.2's six-board pass, and its review | `backup-m4.8.2-presquash` |
| `f8cc32bd` | the `m483` fleet pass | `backup-preland-final` |
| `58e7174e` | the `m484` capture banners | `backup/presquash-m483-m484` |
| `e21167b6`, `1c250bad`, `a1220233`, `367497c2`, `7bdf1067`, `aa38390a` | the M4.8.1 driver-class measurements | `backup/m481-presquash-20260811` |
| `182e0dd2` | scratch console-reclaim instrumentation | `wip/console-reclaim-window-precondition` |

**Two kinds of citation, and only one of them should ever be rewritten.** A hash naming a TREE you
might check out is a reference, and it gets converted to the milestone it belongs to. A hash quoted
as a BANNER is a fact about a string an image printed, and rewriting it would falsify the capture --
those stay verbatim, and this table is what makes them resolvable. `dde73ca` above is the same
situation with a happier ending: it is on `master`.

What makes the `58e7174e` witnesses still good is not reachability but the diff: the two commits
after it touched docs plus one redundant cast, with the armv7m object byte-identical.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `roadmap.md` -- the milestone plan, and the sub-milestone ledger: the only place a number is
  ASSIGNED. This file carries the locked ORDER and cites those numbers.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
