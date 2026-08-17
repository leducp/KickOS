<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

**M4.9.2 IS IN PROGRESS on branch `M4.9.2`, UNMERGED and unpushed.** It converts `volatile` to
relaxed `std::atomic` on every cross-thread field, moves the non-template service-header bodies
into `user/src/`, and writes the four gates `style.md` already claimed. What it is NOT is an
ordering change: relaxed says nothing a second core will honour, so M5 still owns every acquire
and release. `TODO.md`'s M4.9.2 section enumerates what the sweep found.

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
longer has is a route to silicon. **Two things now have no instrument at all**: the segmented
capability table (`KCAP_RUN_CHUNKS > 1`, which no host arm reaches, see the coverage list below) and
the SYSMPU enforcement class.

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

**M4.9.1 IS MERGED (PR #23) and M4.9.2 IS THE LIVE TRACK**, landed-but-unmerged at `10175a7d`.
Its remaining work is the per-chip `arch_console_reclaim` and `arch_console_flush_sync` bodies and
the fleet-wide witness pass; `TODO.md`'s M4.9.2 section is the list. Two of its findings are DEFECTS
rather than gaps, and both are in that list: `esp32` carries a reclaim body with no window body, and
`arch_irq_inject` was missing its `IrqLock` on two backends.

**M4.8.2 (PR #20), M4.8.3 (PR #21) and M4.8.4 (PR #22) are all MERGED, and every silicon obligation
is paid** -- the fleet tables above, the `m483pi` armv6m set, and `m485sq` at the merged tree.
The milestone records are
`docs/design-m4.8.2-host-unit-tests.md` sections 8 and 9, `docs/design-task-layer.md`, and
`docs/design-kill-and-slay.md` (read its section 14, what the design got WRONG, before its section 3).
**The single open item M4.8.4 leaves behind is `rr_interleave` on `rx72m` under
`kickos_services_rx72m_uartirq`** -- and `m485sq` PAID it: that exact list ran `1..104`, 104 ok. The
fix is no longer merely argued.
What follows is what remains, in order.

**The three-track split** -- the M4.8.4 tail, the driver era, a doc
audit, each in its own worktree -- is over on the tail's side. What that experiment taught is worth
keeping, because the next parallel stretch will hit it again: the bench is SERIAL, so a witness
belongs to whichever tree was actually flashed and `-dirty` in a banner is the capture telling the
truth; and tracks that all write `STATE.md` and `TODO.md` collide, so a doc pass goes LAST into any
file another track is still editing. **Measured cost of running them together:** one tree-wide
`-Werror` break that only a FLEET build saw (a half-landed `cap.cc` helper, `defined but not used`,
on the two boards whose config does not reference it), and a stretch where `doc_names` was
legitimately RED while the docs track repaired what the de-poisoned oracle had exposed. Both were
transient and both were caught by a SIBLING track's build rather than by their own -- which is the
argument for keeping the fleet build in the loop, not for serialising.

### The M4.8.4 record

Kept because these are the findings, not the plan. The shape of the milestone was class C first (a
gate that misjudges cannot witness a fix to anything else), then class A, then class B.

**CLASS C IS CLOSED, AND IT KEPT GROWING WHILE BEING CLOSED.** **Do not carry a count from here
either** -- this paragraph said "eight" while `TODO.md` said "ten" for the same work, which is the
tally rot this file warns about, one section up, happening to itself. `TODO.md`'s class C
ENUMERATES them; that list is the authority and this is the shape:
- `check_faultsurvive.sh` MISJUDGED one enforcement class and missed two more. Filed as missing
  two; `armv6m` was the third, and `picopi` is the fleet's only armv6m enforcement unit.
- `panic.ere` never matched the RX or Xtensa terminal reporters, so every ctest
  `FAIL_REGULAR_EXPRESSION` and every `assert_no_panic` was blind to both.
- Three record parsers used `IFS=$'\t'`, a bashism, and `/bin/sh` IS dash here, so they split on
  the letter `t` and `check_seam_defaults` leg 1 was VACUOUS.
- `doc_names`' identifier oracle included a tracked non-markdown file, so any name it mentioned
  stayed valid forever.
- `check_tap_stream.sh` could not see a thread that died the wrong way.
- Every K-seam gate failed to LINK under `sim-telem`, and had since M4.8.2.
- `check_extern_c_linkage.sh`'s prefilter needed `extern` and `"C` on ONE line while the scanner it
  feeds is newline-agnostic, so a wrapped spelling was never handed over.
- `test_classes.txt` pinned its `src` declarations to a file and its `build` ones to nothing. That
  one the gate DISCLOSED rather than hid, and closing it needed a tree-level `<complete>` claim from
  CMake rather than symmetry, since an MCU board legitimately builds none of them.

**EVERY ONE WAS PROVEN BY MUTATION**, and two results are worth keeping for what they say about
mutation itself. The archived `m484mut` record-route mutant is now REFUSED by the repaired
`check_faultsurvive.sh`, so that gate kills a mutant it was blind to when the mutant was made. And
an always-skip mutant on the `task_holds` precondition kills exactly what DELETING the call kills,
so it proves nothing about the guard's shape -- the mutant that earns a count over a boolean is the
sweep clearing its own count.

**CLASS A: five of six closed, and two were mis-filed in ways that mattered.**
- The `sched::wake` guard's MECHANISM is real but "the guard is broken" is NOT: the DECISION is
  provably unchanged in every constructible case, because admission required `t->prio > sweeper` so
  the stale publication is `pick_next`'s own answer and `reschedule()` early-returns on it. Verified
  the other way too -- deleting the `dying` clause reds only the fresh-window arms, which is the
  machine-checked form of "the clause is dead in that window and nothing depends on it there". The
  residue is narrower: a SUPERSEDED publication keeps a `switch_count` it never earned and an RR
  slice armed before it ran. **That residue is UNDETECTABLE IN C** -- no state separates "published,
  switch pended, not fired" from "fired and running" -- so it is reclassified to class B and needs a
  ruling. Also: `arch_switch`'s `from` is IGNORED by all three pending backends, so no context
  corruption is constructible, and the same double publication is reachable from a LIVE thread via
  `task_cancel_group` and the endpoint drain, which no filing mentions.
- The task creator-hold item was filed as a slot LEAK. It is an AUTHORITY ESCAPE: `kill_tag_for_index`
  derives the tag from the pool slot, nothing clears `Task::creator_tag` at the creator's death, and a
  never-freed task has `gen == 0` so its handle is `index + 1`. The successor of a dead creator's slot
  therefore passes `task_created_by` for the predecessor's groups -- it can kill them, and it can
  spawn a child into one and hand that child the group's domain regions. That also refutes the filed
  option "the hold is the creator's for life": that IS the escape.
- `CAP_IRQ` and the console reclaim: inertness needed BOTH priority and chunk alignment, and on
  `esp32-wroom` the alignment condition does not exist at all, because `arch_switch` is synchronous
  in thread context there. One grant-list line from live.
**CLASS A IS CLOSED. The escape is now GATED, and by the HOST route rather than the selftest arm the
analysis proposed.** `kernel/task/task.cc` joined `kickos_kseam`, which cost dropping two stubs and
adding three (`domain_for`, `domain_ref`, `domain_release`) over a fake domain pool -- and NOT the
arena or granule seam, which only `kernel/domain/domain.cc` would have pulled in. `Fixture::task()`
mints through the real `task_create` and `join_task` through the real `task_ref`, so the refcount, the
creator hold and the slot free are the shipping ones and the three arms that used a fixture flag to
answer "did this release empty the group" now get the real answer. Six arms in
`tests/unit/taskdeath/creator_hold.cc`; the escape itself reads
`task_created_by(group, kill_tag_of(successor))` after seating the SAME pool slot, which is what makes
it independent of which slot a reclaim would have chosen. Three mutants, all killed: deleting the
sweep from `exit_current` reds five of the six, dropping its tag test reds the two that say a
stranger's death changes nothing, and a `task_drop_hold` that never frees an empty slot reds the third.
**The seam grew from 19 symbols to 24** (26 under `sim-telem`), re-derived rather than assumed.
**What no host arm reaches is the syscall refusal itself**: `syscall_thread.cc` is outside the seam, so
`-KOS_EPERM` and `-KOS_EBADF` stay the selftest's. `t_task_creator_gate` already runs that wiring on
silicon for a concurrently-live stranger, and the successor case differs from it only in what the
predicate is handed.

**CLASS B: all three are to be FIXED rather than accepted, and the ABI grew a second verb.** Kill
stays cooperative -- 0 means ACCEPTED, death at the next syscall ENTRY -- and **SLAY** is the
forcible half, on the SIGTERM/SIGKILL model. The mechanism is NOT a reaper: no stranger ever runs
another thread's `cap_teardown`. The victim runs its own, because the seam rebuilds `next->ctx`
inside `switch_to` before `arch_switch`, so the thread resumes at an exit stub of its own.
Two premises died on the way there, and both were the maintainer's-session reading rather than the
tree's: `arch_fault_redirect_to_exit` CANNOT be reused (it is not relocatable to a saved context on
any backend, and on armv7m and rxv3 it reads AND CLEARS sticky fault status, so calling it off a
fault destroys the reporter's evidence) -- `arch_context_init` is the seam that already fabricates a
frame with privilege on six backends. And the termination argument is NOT the RR slice timer: the
clock is TICKLESS, `ktime_rearm` disarms entirely for a FIFO current thread with no sleeper, so an
all-FIFO image has no periodic interrupt. The real argument is stronger -- on one core a target that
is not the caller is never RUNNING, so READY and BLOCKED cover every live state.
The design gate is `docs/design-kill-and-slay.md`, which records the ABI, the mechanism, the reaper
as the REJECTED alternative, and six open questions. Four non-reorderable steps, **zero `.bss` on
every one** (`bool cancelled` becomes `uint8_t cancel_kind` at the same offset, so `sizeof(Thread)`
is unchanged by construction, which is what clears `microbit`). S1 -- the fault-path stack reset plus
the rxv3 band NARROWED rather than deleted -- closes class B item 1 without slay and goes first,
alone. Deleting that band instead would make an overflow die thread-scoped on rxv3 while armv7m keeps
escalating through the CFSR `0x1818` frame-validity mask, diverging from four witnessed
`faultsurvive_ovf` captures.

**ALL FOUR STEPS HAVE LANDED, and the record of what the design got WRONG is
`docs/design-kill-and-slay.md` section 14.** Read that before section 3 of the same file.
The three that matter:
- **`lx6` needs NO extra line in the seam, and the design's instruction would have been a bug.**
  `arch_context_init` already writes `resume_kind = KICKOS_RESUME_IRQ`, which is what a
  FABRICATED frame is; forcing COOP as section 3.3 says would send the switcher down the `retw`
  path onto an interrupt frame. That write is load-bearing, not incidental: a thread that blocked
  cooperatively carries `KICKOS_RESUME_COOP` and a rebuild has to overwrite it.
- **The self-slay hole S3 was gated on does not exist, and never did.** The moved-`current`
  window is entirely inside the kernel under an `IrqLock`, so `prev` cannot issue a syscall from
  it; at every syscall ENTRY `kernel().current` IS the caller. Mutation agrees -- deleting the
  refusal reds a selftest arm, where the design predicted the mutant would survive. That matters
  because class A closed by RULING the residue undetectable in C, so the gate was on an event
  that was never going to happen.
- **A new `WaitKind` costs four arms, not one.** `WAIT_TASK_EMPTY` needs the exit sweep's wake,
  `thread_abort_park`'s unwind and `ktime_on_timer`'s expiry beside the enum value. Both defaults
  it would otherwise fall through are fail-closed (a `KICKOS_UNREACHABLE` and a `kpanic`), so the
  omission would have been loud -- but it is work the estimate did not carry.

**The footprint promise held, measured against a pristine pre-S1 tree**: `microbit`'s
selftest `.bss` is 6512 before and after and its `.data` 396, `sizeof(Thread)` is 256 / 264 /
2480 on microbit / picopi / sim, and its declared skip set did not grow. `.text` is +3552,
which is where the two syscalls, the six seam wrappers, the stub and five new selftest arms went.

**CLASS B ITEM 3 -- the rxsci relay's block -- IS RULED, AND THE RULING IS A REFUSAL PLUS A
COLLAPSE.** The filed fix was a THIRD memory scope: seat a member in the group with no domain
regions. Refused, because `docs/design-task-layer.md` defines a task as the set of threads sharing
ONE memory domain, so a member that shares the task but not its domain makes the definition false --
two answers to "what memory do this task's threads see" is precisely the second truth the tiebreaker
forbids. The right decomposition is a task of its own, and the only thing stopping that is that a
task is also the kill group; separating "shares memory" from "dies together" is M5-scale.
**What landed is the deletion of the declaration that lied.** `drv::Thread::mem_grant`'s only reader
was an OR-reduction into the group's grant, and it equalled `arg == KOS_DRV_ARG_BLOCK` in all
TWELVE production descriptors, so it was a second truth twice over. `bring_up` now hands the task the block
whenever there is one -- provably inert fleet-wide, because every block-owning descriptor already had
a thread declaring it. Leg L4 grew the converse arm: a block NO thread reads is now a compile error,
which is the widest ask a descriptor could make and the one nothing else would have caught.
Three mutants killed, one of them at COMPILE time by the `static_assert` beside the new fixture.
**The residual is stated at three declaration sites** (`Descriptor::block_size`, `kos_drv_arg`, and
rxsci's relay itself): every thread of a block-owning driver sees the whole block whatever its
argument, because a task owns exactly one `Domain` and a member may bring no grant of its own.
`rx72m` has no emulator and no CI gate, so the rxsci edit is BUILD-VERIFIED ONLY.

**M4.8.4 tail gates**: `sim` 223/223, `sim-telem` 225/225, `qemu` 31/31, `qemu-riscv` 25/25, fleet
builds 10/10 (`microbit picopi frdmk64f xmc4800-relax rx72m esp32c6-wroom esp32-wroom pizero2350
f302nucleo bluepill-c8`). `microbit`'s selftest `.bss` is 6512 and `.data` 396, unmoved, with
`_ebss` == `__kickos_ram_start` == `0x20001b00` as ever; `sizeof(Thread)` is 256 / 264 / 2480 on
microbit / picopi / sim. **No selftest arm was added, so no `_tap_arms` count moved and no board's
skip set could change** -- the whole of class A's gate is host-side.

**The one regression M4.8.4 introduced is fixed in code and NOT yet witnessed.** The creator-hold
sweep is now skipped outright while `Kernel::task_holds` is zero, which is every image that never
calls `kos_task_create`. What the host cannot answer is the thing that found it: `rr_interleave` on
`rx72m` under `kickos_services_rx72m_uartirq`, where 3 of 3 runs printed `rr order: AABBAB`. Until
that capture exists the fix is ARGUED. Gates at this tree: `sim` 224/224, `sim-telem` 226/226,
`qemu` 31/31, `qemu-riscv` 25/25, fleet builds 10/10. **The counter is free by MEASUREMENT, not by
argument**: all nineteen `microbit` images and `qemu-telem`'s selftest are byte-identical in
`.data`, `.bss`, `_ebss` and `__kickos_ram_start` against a pristine `git archive` tree, because two
bytes of padding sit before `Kernel::sleepq` in both telemetry postures on a 32-bit target.

**The same shape one layer down is now fixed too, and it runs on EVERY thread exit.** `cap_teardown`'s
`CAP_IRQ` pre-pass -- the one that must release every line before the chunked loop opens its first gap
-- scanned the whole capability table under ONE unbroken `IrqLock`. `Thread::cap_irq_live` counts the
entries a thread holds, so the pass reads none at zero, which is every thread but a driver's IRQ
thread. **The no-chunking property is what made a per-thread count the only acceptable answer**: the
loop still sits inside the single masked window that bumps the teardown depth, because a gap inside it
would be a moment when a thread with a counted depth still holds a line, and both console reclaim
sites read that. A per-KERNEL count would have been cheaper to place and WRONG here -- one IRQ driver
anywhere makes every other thread's exit scan again, and `kickos_services_rx72m_uartirq` is that
image. The byte is free: it takes the last padding byte before `Thread::quantum_ns`, so
`sizeof(Thread)` is unmoved at 256 / 264 / 2480 and `microbit`'s selftest is unchanged in `.bss`
(6512), `.data` (396), `_ebss` and `__kickos_ram_start` (both `0x20001b00`). Gates at this tree:
`sim` 225/225, `sim-telem` 227/227, `qemu` 31/31, `qemu-riscv` 25/25, fleet builds 10/10. Five
mutants killed, including the pre-existing chunk-it and delete-it pair. **SILICON OWED, and it is the
same capture as the creator-hold fix**: `rr_interleave` on `rx72m` under
`kickos_services_rx72m_uartirq` is the only instrument that has ever shown this class.

**Gates at the slay tree**: `sim` 214/214, `sim-telem` 216/216, `qemu` 31/31, `qemu-riscv` 25/25,
`microbit` 19/19, fleet builds 10/10. All TEN fleet images carry both `arch_ctx_redirect` and
`kickos_thread_slay_exit` by `nm`, which is the only corroboration available for the six boards
with no runner (and note the RX toolchain prefixes its symbols with an underscore, so a grep
written for the ARM output reads rx72m as ABSENT).
Twenty-one mutants applied, built and run; eighteen killed. **Three survive and each is named
in section 14.5** -- the idle/privileged refusal and the caller-is-a-member refusal are both
fail-closed guards on shapes userspace cannot construct today (idle is a static TCB outside the
pool, root is unprivileged, and a member cannot be its own group's creator), and dropping the
creator hold early is harmless only until a third thread recycles the slot in a teardown chunk
gap. **One mutant survived a fully green run and repairing the GATE is the finding**: an
unprivileged rebuild faults the stub, the isolation path catches it, the victim still dies
windowless, and every plan/case/directive check reconciled -- the only trace was a
`=== THREAD FAULT ===` line nothing read. `tests/integration/check_tap_stream.sh` now refuses one.

1. **M4.8.3 -- MERGED (PR #21). A RECORD, not an item**, and its tail M4.8.4 is merged too,
   `docs/design-task-layer.md`. A task is a set of threads;
   the address space attaches to Domain, not Task. `sizeof(Thread)` is unchanged across the WHOLE
   milestone -- 256 microbit, 264 picopi, 2480 sim, re-measured at the 9.5 tree -- and so is
   `microbit`'s `.bss`: `Task` was REPACKED for 9.4 rather than grown, so the skip set 9.3 cost that
   board did not grow again.
   - **9.3**: a `Task` owns the `Domain*`, one task per thread implicitly, `Thread::domain` became
     `Thread::task`.
   - **9.4**: `kos_task_create` makes an EMPTY group holding a domain built from its own grant, and
     `kos_thread_params::task` seats a member. The gate is CREATORSHIP, not possession: a task handle
     is a plain word (generation over a BIASED index, so the all-zero word means "no task" and every
     one of the 230 existing spawn sites keeps its meaning unedited), and the cap codec is untouched.
     **A DEV window is now the asking THREAD's region and never its task's domain**, so `domain_for`
     takes no MMIO argument, `dev_window_free` scans live THREADS and skips a `dying` one, and the
     window's Rule 7 admission moved to the spawn boundary where the grant is asked for.
   - **9.5**: a member's death ends its group, and `kos_task_kill` does it from a supervisor. The
     cancel is TOTAL over `WaitKind` and the DEATH POINT is the next syscall ENTRY -- which is what
     reaches a thread parked in `sem_wait` or `sleep`, neither of which has an error return to
     cooperate through. **0 still means the request was ACCEPTED, never that the thread is gone**: a
     thread that never re-enters the kernel is unreachable without preemption.
   **The drivers opt in through ONE call site**, `drv::bring_up`, so all twelve are converted by
   editing the generic service; `ThreadSet` is gone and the hand-rolled group kill with it. Two costs
   worth knowing: `rx72m/rxsci`'s relay thread now sees the ring block it declared no grant for (the
   block is the GROUP's region, and M4.8.4 DELETED the per-thread flag rather than honour it -- see
   class B below), and the sim's WINDOWED
   console posture deliberately keeps its window thread OUT of the driver's group, because a foreign
   holder of the registers is the only shape in which the deferred console reclaim is observable.
   **The banners now say `thread '%s'` where they said `task '%s'`**, because with a Task in the tree
   naming a thread and saying "task" is a second truth. Archived silicon captures keep the old
   string: editing those would falsify what a past image printed.
   **SILICON AT THE M4.8.3 CLOSE**, TAG `m483` plus `m483c6`, in the same pass as M4.8.2's and recorded in
   the fleet table above: seven boards, the four task arms (`task_handles`, `task_member_refusals`,
   `task_creator_gate`, `task_group_kill`) ok on every one of them, and `rx72m`, `esp32-wroom` plus
   `esp32c6-wroom` run
   a second time under their `_uartirq` lists so the converted `drv::bring_up` is in the image rather
   than absent by default. The death point `tests/unit/taskdeath` cannot see is now witnessed on
   silicon on FOUR boards and three arch classes -- but by `faultsurvive`, not by the selftest, and
   only where `KICKOS_FAULT_ISOLATION` is 1. **`rx72m` (rxv3) joined that set in this milestone and is
   witnessed on silicon** (see the rxv3 section above); on `esp32-wroom` (lx6) the app is still not a
   target, a fault stays system-terminal, and that capture witnesses the group-death half alone. **`rv32imac` was the third arch 9.5 changed and it is now witnessed BOTH ways**, group kill
   and fault kill, on `esp32c6-wroom`.
2. **M4.9.1 -- USB CDC console. THE ONLY LIVE TRACK**, continuing M4.6.2, on `M4.9.1-usb-cdc`
   (unpushed). The ruled contract is **keep UART blocking semantics AND add an `O_NONBLOCK` mode
   fleet-wide, with the non-blocking path reporting HOW MANY BYTES IT WROTE so the caller paces its
   own retry**, and it is IMPLEMENTED. One policy function, `console::mode_apply` in
   `user/include/kickos/sys/console_ring.h`, decides it for every transport, so there is no second
   copy: the five silicon UART consoles inherit it through `uart_service.h`, and USB CDC both
   defaults to and REQUIRES `KOS_UART_F_NONBLOCK`, refusing `-KOS_ENOTSUP` on a request to clear it
   because no IN token is issued until a host opens the tty and a paced write there is unbounded.
   `docs/reference/console.md` states the op table and the policy; that wire ABI had no Reference
   home before.
   **The "~2.7 KiB dropped at teardown" figure this entry used to carry is RETRACTED** -- a
   capture-harness artifact at the HEAD of the stream, not a device loss.

   **WITNESSED, and each capture belongs to the tree named:**

   | board | app | tree | result |
   | --- | --- | --- | --- |
   | `pizero2350` (RP2350) | `usbcdcwit` | `e0ab9cf9` clean | 8192 of 8192, `drop=0`, `maxzero=569` -- the ring went FULL and the short-accept retry recovered it, which is the contract working |
   | `picopi` (RP2040) | `selftest` under `_usbcdc` | after the IRQ fix | `1..104`, 0 not ok, 0 skip, `# all tests passed` |
   | `teensy41` | `selftest` | `a4a3d8dc` clean | `1..104`, 0 skip, 0 partial, enforce, through `check_tap_stream.sh` |

   **`teensy41` had never been in the bench chain and now is** (console row, HalfKay branch, rig
   key). It is the fleet's only Cortex-M7 and had no capture since the ERR011573 work. Its first
   HalfKay load fails and the second succeeds often enough that it is retried automatically, both
   loads bounded: `teensy_loader_cli -w` blocks until a HalfKay device appears, and only a button
   press brings one back, so an unattended pass would otherwise hang forever.

   **A CONSOLE THAT IS THE DEVICE LOSES THE HEAD OF EVERY CAPTURE**, by construction: nothing can
   listen until the image has booted and enumerated, and the banner and the `1..N` plan line are out
   before that. `bench-capture.sh` arms its reader BEFORE the flash and spins on the path, because
   waiting to see the device and then arming spends about as long as the app lives and captures
   nothing. `usbcdcwit` reprints `kickos_build_commit` where the host is certain to be listening, a
   witness that cannot name its tree being no witness. `check_tap_stream.sh` gained
   `TAP_HEADLESS_LAST`, which brackets the tail against a caller-named last arm and requires the arm
   numbers to step by exactly one; it states which arms it does NOT cover.

   **THE TEN-ANGLE REVIEW RAN and its three findings are fixed** (`338ff9dc`, `2ecf03d8`). Worth
   keeping: the rp2350 IRQ-line fix had REINTRODUCED its own defect on `esp32c6`, whose
   `UART0_TX_LINE` is 16 and collided with the arm's new fallback line; `KOS_UART_SET_MODE` had no
   end-to-end coverage on the substrate all five consoles share, only `mode_apply` as a pure
   function; and `stats.tx_dropped` had two writers on two threads.

   **STILL OPEN**: bulk OUT has never been exercised at all, `Shared::configured` does not clear on
   a bare unplug because no backend arms a disconnect or suspend source, and `teensy41`'s USB
   backend is absent by construction. **The three captures predate the last three commits**, so they
   want retaking before the milestone closes -- a witness is valid for a TREE.
   Its witness app `user/apps/common/usbcdcwit` is gated on `KICKOS_SERVICE_LIST MATCHES "_usbcdc$"`,
   so it builds under EITHER RP list and under no default configuration of any board: a routine
   green sweep says nothing about this branch, and **two boards can carry that witness, not one**.
3. **M4.9.2..N -- the fleet-wide witness pass**, and the per-chip `arch_console_reclaim` bodies.
   **Nothing in-tree can catch a wrong `arch_mpu_region_pow2()` literal in a backend**
   (`cmake/boot_arena.cmake` scrapes the same file the link resolves), so `rx72m` silicon is the only
   check on that class for the RX MPU.

Captures and records already stamped `M4.6.2` keep that name: a measurement is never renamed.

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
- **`KCAP_RUN_CHUNKS > 1` reaches no host arm.** The K-seam fixture compiles the SIM posture only,
  so the segmented capability table is `frdmk64f`-only. **It is witnessed again at `m492k`** (see
  above), because the fleet ruling is about UNATTENDED passes and a person can clear the OpenSDA
  dialog in ten seconds. So the instrument exists whenever someone is at the desk, and the durable
  fix is still to teach the K-seam fixture the segmented posture rather than to find another board.
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
- **A per-chip `arch_console_reclaim` body exists only on `mk64f`, `xmc4800`, `esp32` and
  `esp32c6`** -- FOUR, not three -- so elsewhere a driver death flips the state and the polled
  route works but the DEVICE is whatever the dead driver left. Per-chip bodies are fleet work;
  `roadmap.md`'s sub-milestone ledger says which number that is.
- **`arch_console_flush_sync` is a DEVICE drain and only `mk64f` and `xmc4800` have a body**, so on
  every other board `kickos_terminate` empties the console RING and then stops the core with
  whatever is still in the UART FIFO. `arch.h` used to document the seam as a clock-retune hook
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
