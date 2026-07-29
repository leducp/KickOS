<!-- SPDX-License-Identifier: CECILL-C -->
# Unprivileged root -- start unprivileged holding capabilities

> **Status: ACTIVE** -- stages 0 and 1 have landed, and **stage 2 is done**: the whole declared
> set of five boards boots an unprivileged root and is witnessed on silicon, covering every
> enforcement backend -- `xmc4800-relax` (PMSAv7), `esp32c6-wroom` (RISC-V PMP), `pizero2350`
> (PMSAv8), `rx72m` (RXv3) and `f411disco` (PMSAv7, the last one, 2026-07-29). The per-board
> captures are in `reference/boards.md`. `frdmk64f` waits for stage 3. Stages 3-4 are planned and
> hardware-gated. See `design/README.md` for the marker taxonomy. The actionable checklist is
> `TODO.md`; this record is the reasoning behind it, which `TODO.md` does not carry.

Root is the kernel's first application thread: `kmain` creates it, it runs the app and
library constructors, then calls `kickos_init_entry`. It **was** privileged for its entire
life, holding every authority the kernel can grant for that whole time -- and on a board that
has not flipped it still is, because `KICKOS_ROOT_PRIVILEGED` is in the tree and defaults ON.
That knob is **decided for deletion, with no replacement**: unprivileged root becomes the only
posture, on every board, and there is nothing left to configure. Section 4 carries that decision
and what it costs; the code still has the knob.

This record is why the fix is **start unprivileged holding capabilities** rather than **start
privileged and demote**, what that costs, and -- the part worth writing down -- the specific
places where it does not work.

---

## 0. Outline

1. The decision, and the design it superseded
2. Why it needs no new assembly on any port
3. Why the region set needs no recomposition
4. What was deleted rather than deferred
5. The authority capability: shape, seat, and the arithmetic behind it
6. The actual win: revocation
7. Stage plan, and what stages 0 and 1 landed
8. The three blockers
9. Limits, including the boards where this does not work
10. Verification: what witnesses what, and what nothing witnesses

---

## 1. The decision, and the design it superseded

The earlier plan was `KOS_SYS_DROP_PRIV`: root boots privileged, does its bring-up, and then
demotes itself before calling `main`. That needed a per-arch backend to change a *running*
thread's privilege, a `thread_regions_recompose` to rebuild the MPU region set at the moment
of demotion, and it put Xtensa last because Xtensa has no ring split to demote across.

The replacement is simpler and strictly stronger: **root is created unprivileged, holding
capabilities for the authorities its bring-up needs.** There is no demotion, so there is no
mechanism to build for it, no window during which the region set is half-recomposed, and no
per-arch asymmetry.

The direction matters. Demotion is a transition, and a transition is a thing that can be
skipped, forgotten on one port, or raced. Starting unprivileged is a *property of the first
frame*, decided once at `thread_create` and never revisited.

## 2. Why it needs no new assembly on any port

Because every ISA with a ring split already encodes thread privilege in the fabricated first
frame and restores it on the first switch-in. Nothing new has to be written; `thread_create`
already passes `privileged` down to `arch_context_init`, and every backend that has a mode to
set already honours it:

| Port | Where privilege lives in the fabricated frame |
|---|---|
| armv7m | `ctx.npriv` **and** `ctx.resting_npriv` (`arch/arm/armv7m/arch_armv7m.cc`, `arch_context_init`) |
| armv6m, Cortex-M0+ (`picopi`) | the same two fields (`arch/arm/armv6m/arch_armv6m.cc`), read by `switch.S` at fixed offsets 4 and 8 |
| armv6m, Cortex-M0 (`microbit`) | the same two fields are written, and the hardware discards them: ARMv6-M's Unprivileged/Privileged Extension is optional, separate from the MPU extension, and the Cortex-M0 does not implement it (Cortex-M0 TRM DDI0432C, *Modes of operation and execution*: this processor does not support different privilege levels, software execution is always privileged) |
| rv32imac | `mstatus.MPP` in the fabricated frame -- M-mode for privileged, U (0) otherwise (`arch/riscv/rv32imac/arch_rv32imac.cc`) |
| rxv3 | `PSW_THREAD_USER` in the fabricated PSW (`arch/rx/rxv3/arch_rxv3.cc`) |
| lx6 (Xtensa) | nothing to set: the LX6 has no ring split, so the flip is a no-op here |
| sim | nothing set and nothing recorded: `arch_context_init` takes `privileged` and discards it (`arch/sim/sim.cc`) |

Four consequences follow.

**On `microbit` the kernel's privilege bit is a fiction.** The nRF51822 is a Cortex-M0
(`boards/microbit/board.cmake:12`, `-mcpu=cortex-m0`), so `arch_context_init` seeds `npriv = 1`
(`arch/arm/armv6m/arch_armv6m.cc:98-104`), `switch.S` writes it into `CONTROL`
(`mrs`/`bics`/`orrs`/`msr`, lines 66-71), and the `msr` is architecturally discarded: **a thread
the kernel believes is unprivileged runs privileged.** The fleet's other armv6m board, `picopi`,
is a Cortex-M0+ (`boards/picopi/board.cmake:12`, `-mcpu=cortex-m0plus`) and does implement the
extension, so one arch backend spans a core with a privilege ring and a core without one, and
nothing in the tree distinguishes them. `microbit` therefore witnesses the capability gates and
nothing about the CPU ring.

**Xtensa comes along free instead of last.** Under the demotion design Xtensa was the hard
case, because there is no privilege level to drop to. Under this design it is the trivial
case: it has no ring split, so an "unprivileged" thread there is the same thread, and the
capability gates still narrow what it may ask the kernel to do. That is a real gain even
without hardware enforcement -- the authority check is kernel logic, not an MPU feature.

**The armv7m `resting_npriv` field is the tell.** It exists because privilege is a *resting*
property of a thread that the syscall path temporarily leaves and returns to. A design that
changes it mid-life has to update the resting value too, in asm, on every port. A design that
sets it once does not.

**The sim has no privilege axis to set.** A host `ucontext` has no ring, so the backend takes
the flag and drops it on the floor. What stands in for privilege there is the *memory* posture:
the thread's `mprotect`'d region set, plus a per-context `raised` counter that grants the whole
arena for the duration of a syscall and re-grants it if the thread is resumed mid-syscall
(`arch_syscall`, `guard_apply_current`). So the sim collapses CPU mode and memory posture into
one thing and cannot tell a privileged thread from an unprivileged one holding a permissive
region set. Section 10 is where that bill comes due.

## 3. Why the region set needs no recomposition

`thread_create` composes the MPU region set once (`kernel/thread/thread.cc`), from a privilege
that never changes afterwards:

- privileged: the whole arena, with the backend's permissive background covering code, kernel
  data and stack -- one region suffices.
- unprivileged: `arch_domain_static_regions` (app code RX + app static data RW) + the domain's
  regions + the thread's own private stack.

`thread_regions_recompose` existed only to rebuild that set at the demotion instant. With no
demotion there is no instant, so the function has no reason to exist. This is why the change is
mostly *deletion*.

## 4. What was deleted rather than deferred

`thread_regions_recompose`, `KOS_SYS_DROP_PRIV`, its per-arch backends, and the Xtensa-last
sequencing are **deleted, not deferred**. They were machinery for managing a transition this
design does not have; carrying them as "later" would imply they are still wanted.

`drop_priv` survives only as a **contingent, much smaller** item. It is the one mechanism that
gives "privileged bring-up, then self-confinement for life", which is exactly what the blocked
bring-up bodies in section 8 want. It is in scope only if the privileged-write seam family
(section 9) proves insufficient for them.

### `KICKOS_ROOT_PRIVILEGED` goes too, with no replacement

**Decided, not yet applied: the knob is deleted outright.** No bypass, no porting helper, no
tier. Where the CPU has a privilege ring KickOS uses it from the first frame; where it has none
the kernel's authority checks still hold and there is nothing to configure either way. A porter
bringing a board up may flip a thread's `privileged` attribute locally while working, and that
must never land in master. The reason is that this is a kernel and not an application: a knob is a
posture someone can ship by accident, and the posture this one permits is "root holds every
authority for the life of the system". Fewer knobs, fewer misconfigurations.

**The safe default flips with it.** `ThreadAttr::privileged` is `true` today
(`kernel/include/kickos/thread.h:164`), so an attribute struct that forgets the field mints a
fully-authorized thread. It becomes `false`, with privileged spelled out at the one site that
needs it, and a forgotten field can then no longer silently mint privilege. That costs nothing at
the sites that exist, because the kernel builds exactly three `ThreadAttr`s: `idle` already spells
`privileged = true` out (`kernel/init/kmain.cc:230`), root drops its knob derivation and takes the
new default (`kernel/init/kmain.cc:250`), and the spawn syscall assigns the field from its caller's
request either way (`kernel/syscall/syscall_thread.cc:334`).

**The deletion cannot be scheduled before stage 3, and that is mechanical rather than a
preference.** `frdmk64f` under `KICKOS_HAVE_MPU` defaults to the `kickos_services_frdmk64f` service
list, which is listed in `KICKOS_SERVICE_LIST_ROOT_MMIO` and therefore refused by a configure-time
`FATAL_ERROR` whenever root is unprivileged (`CMakeLists.txt:404`). With the knob gone that refusal
has no posture left to be conditional on, so it fires unconditionally and the board's enforcement
build **stops configuring** -- which is the `build-boards-mpu` CI job, not a bench inconvenience.
Stage 3's `arch_periph_enable` is what retires the root MMIO writes that put that list on the
refusal list in the first place. So the order is stage 3, then the deletion; section 7 has the two
smaller build consequences that travel with it.

**The invariant that leaves is "exactly one privileged thread, and it is `idle`".** Idle has to
stay privileged because the instruction it exists to execute is not portably available
unprivileged: RXv3 `WAIT` is privileged (RXv3 ISA UM section 1.4.3) and RISC-V only optionally
permits U-mode `WFI`. Section 9 carries that argument. Anything else privileged after boot is a
bug, and section 9 is also why none can appear -- spawning a privileged child needs a privileged
caller, and idle runs no app code.

## 5. The authority capability: shape, seat, and the arithmetic behind it

The gates that read `privileged` now call `cap_check_authority(caller, AUTH_*)` and nothing
else. The privileged-implies-everything arm lives *inside* that function, so the rule is stated
once rather than at every site -- which is the stronger property, and precisely what makes the
stage-1 conversion behaviour-neutral: there is no call site left that could encode it
differently. The authority is a capability, `CapType::CAP_AUTHORITY`.

**It is poolless.** `CapEntry.obj` is unused and there is no refcount, because there is no
object behind it -- the capability *is* its rights bits. That single fact drives two API
consequences worth stating, because both are easy to get wrong:

- It resolves by reading its reserved slot, and **never** through `cap_resolve_e`, which would
  try to resolve `obj` in an object pool and correctly hand back `nullptr`.
- `obj_ref_inc` and `obj_ref_drop` need explicit no-op arms. Both `default:` cases assert, so a
  missing arm is a debug trap rather than a silent leak -- which is the behaviour you want, and
  the reason the arms are explicit rather than relying on the default.

**Five rights bits, and that is the entire budget for the life of the type.** `CapEntry.rights`
is a `uint8_t` with three bits used (`CAP_WAIT`, `CAP_SIGNAL`, `CAP_TRANSFER`). The five free
bits become `AUTH_MEMORY`, `AUTH_PINMUX`, `AUTH_CLOCK`, `AUTH_IRQ`, `AUTH_DEVICE`.

The split sits at bit 3 rather than overlapping the object rights, deliberately. `CAP_TRANSFER`
in particular is read *type-agnostically* at the delegation site, so reusing bit 0 or 1 for an
authority would give one bit two meanings depending on a field the reader has to remember to
check. `CapEntry` is a frozen 8-byte ABI, so the field cannot grow: a sixth authority has to
come from **merging two existing bits** (`AUTH_PINMUX` + `AUTH_CLOCK` into one chip-config bit
is the natural pair -- both are one-shot bring-up configuration), never from adding one.

**It is seated at `KOS_CAP_AUTHORITY` (index 2), which was already reserved.** So it costs
**zero dynamic capability slots on every board**, including the four with only 9 handles. That
is the whole reason it is a rights-bearing cap at a reserved index rather than a new object.

**It is seated without `CAP_TRANSFER`, which makes it non-delegable.** Not merely
undelegated -- the delegation site requires `CAP_TRANSFER` on the source cap, so an authority
cap can never be copied into a child table, and index 2 therefore has exactly one writer: the
kernel. This closes the forgery question completely and defers the delegation-packing collision
in section 9 rather than walking into it.

**Per-instance capabilities were refuted on arithmetic, not on taste.** The alternative was a
capability per muxable pin (or per clock, per line) instead of one bit per authority *class*.
There are roughly 100 muxable pins on the larger parts against a 16-slot capability-table
ceiling. It does not fit, and it does not nearly fit, so the class granularity is forced.

## 6. The actual win: revocation

Confining root's *memory* is worth something, but it is not the main prize. The prize is that
**an authority can be dropped**.

Today root holds every authority for its entire life: it pin-muxes the board during bring-up
and then keeps `AUTH_PINMUX` forever, through the whole run of the application. With the
authority in a capability, root can drop `AUTH_PINMUX` after bring-up and before calling
`main`, and the kernel will then refuse a pinmux request from it -- including one made by
application code that has gone wrong.

That is stage 4's `kos_cap_narrow`, and it is the app-facing point of the whole exercise.
Without it an unprivileged `main` can still ask the kernel to do every privileged thing there
is, which would make the flip a memory-isolation change and nothing more.

## 7. Stage plan, and what stages 0 and 1 landed

**Stage 0 -- three independent prerequisites. LANDED.** All three were real bugs, latent only
because privileged callers bypass the checks:

- **The argv handoff lived on the boot stack.** `root_entry` read `argc`/`argv` from a `kmain`
  frame local, and the boot stack is outside the arena, so an unprivileged root would fault on
  that read before its first statement on every enforcing board. Now `kickos_init_args`, in
  `libkickos_user.a` -- an archive every enforcement linker script routes into the
  `.appdata`/`.appbss` grant. Deliberately not the init provider's archive: `kmain` references
  the object unconditionally, so a build naming its own `KICKOS_INIT_PROVIDER` must not be able
  to take the definition away. **The sim cannot reproduce the original fault at all**, because
  `kmain`'s frame is host stack there.
- **Ending the system called two kernel functions directly.** `console_tx_flush_sync` walks the
  kernel-side console ring and `arch_shutdown` is a privileged chip operation, so neither is
  reachable from an unprivileged thread. Both now sit behind `KOS_SYS_SHUTDOWN` (36).
- **`user_writable_ok` had no static-data arm** where `user_readable_ok` has
  `arch_user_text_readable`, so on any backend that models no static-data region an
  unprivileged thread's writable set was its own stack alone. Fixed with
  `arch_user_data_writable`.

Measuring that last one changed the plan: the hole is **not** confined to the five chips with no
MPU *backend* (stm32f103, stm32f302, nrf51, sam3x8e, esp32) -- and only four of those five lack an
MPU on silicon. The AT91SAM3X8E **has** one (SAM3X/SAM3A datasheet features: Cortex-M3 revision 2.0
up to 84 MHz, a Memory Protection Unit, and Thumb-2); what KickOS lacks is an `mpu.cmake` backend
for it, an unwritten port rather than a hardware limit -- and the `due` unit is retired from the
bench, so the backend is both unwritten and unwitnessable in practice. Of the other four,
`stm32f302` names the R8's `x8` line, which has no MPU (the F302xB/xC line does have one),
`stm32f103` and `nrf51` have none, and the LX6 `esp32` has no unit KickOS drives.
**The host sim has it too**, despite building `KICKOS_HAVE_MPU=1`, because its globals live in the
host image rather than the mprotect'd
arena. So the fix could not key on `KICKOS_HAVE_MPU` alone and the sim carries its own arm.
Worse, the selftest had already *worked around* the bug -- `ep_recv_worker` carried a comment
explaining that its recv buffer had to be a stack local because a global "would be rejected on
the sim / no-MPU backends". The bug was known, routed around in test code, and never filed.

**Stage 1 -- the authority capability, root still privileged. LANDED.** The type, the five
bits, `cap_check_authority`, and the gates converted. Behaviour-neutral by construction:
root is privileged, so every gate takes its privileged arm exactly as before.

**There are nine authority decisions in the kernel, and stage 1 converted eight of them.** The
eight are seven cases in `kernel/syscall/syscall.cc` plus the MMIO grant in `thread_spawn`. The
ninth was missed: `grant_region_admissible`'s DEV arm (`kernel/grant/grant.cc`, Choice 5A) went
on reading raw `Thread::privileged`, and it is not in the list of surviving reads below because
nobody knew it was there. It is invisible while root is privileged, and it sits directly on the
console-handover path, so stage 2 found it the hard way: an unprivileged root holding
`AUTH_MEMORY` clears the `thread_spawn` gate, is refused here, and the board goes dark. Fixed on
the stage-2 branch, as `m4.5.1: gate the MMIO grant on AUTH_MEMORY, not on the caller's
privilege`. The lesson is to enumerate the *decisions* rather than count the call sites: a count
that is right on the day it is written is exactly what lets the next one hide. (The counts above
are themselves dated to stage 1: the set has since grown -- `KOS_SYS_MEM_SELF_GRANT` and the
spawn-time authority-narrow check joined on the same branch. The enumeration that stays true is
the grep: every gate is a `cap_check_authority` call site.)

The reads of `Thread::privileged` that survive are exactly the set this design keeps:

- the privileged arm inside `cap_check_authority` -- "privileged implies every authority",
  stated once;
- **spawn-a-privileged-child**, which is deliberately *not* a capability, because holding it is
  equivalent to holding everything forever;
- the child-privilege selection of memory posture in `thread_spawn`;
- the confused-deputy bypass in `syscall_mem.cc`.

**Stage 2 -- flip per board**, behind a build-enforced `KICKOS_ROOT_PRIVILEGED` knob, default
ON. **Not a weak symbol**: opting out of the boundary must be visible in the board's build, not
silently satisfied by a link-time override. Five boards are flipped and silicon-witnessed,
covering every enforcement backend: `xmc4800-relax` (PMSAv7, with a console-only service list --
its `xmcuart` bring-up is pure syscall), `esp32c6-wroom` (RISC-V PMP), `pizero2350` (PMSAv8),
`rx72m` (RXv3) and `f411disco` (PMSAv7). That is the whole declared stage-2 set. The captures are
in `reference/boards.md`.

With that set witnessed the per-board phase is over, so the knob has no remaining job and is
deleted (section 4). Three consequences sit in the build rather than in the kernel. The binding one
is the `frdmk64f` configure refusal, which orders the deletion after stage 3 -- section 4 states it,
because whoever schedules the deletion needs it there rather than here. The other two are smaller:
`xmc4800-relax` stops picking between two service lists by posture and always takes the console-only
one, and the confinement gate stops being opt-in (section 10).

`f411disco` came last because what stood in the way was a **pre-existing bench debt rather than
flip work**: PMSAv7 enforcement had never been witnessed on that board at all, so a flip there
would have had no enforcing baseline to be discriminating against.

`frdmk64f` is **not** a stage-2 target and waits for stage 3. Its `k64uart` and `k64dspi` PACR
writers (`AIPS0_PACRN` at `0x4000_0064`, `AIPS0_PACRF` at `0x4000_0044`) both fall inside
`arch_reserved_blocks`'s AIPS0 entry (`[0x4000_0000, 0x4000_1000)`), so no grant can ever reach
them and `arch_periph_enable` is the only way in.

**Stage 3 -- `arch_periph_enable(base)`**, weak `-KOS_ENOSYS`, gated `AUTH_DEVICE`, covering
"ungate the clock and drop supervisor-protect for the block at this base". Implemented for K64F
(`SIM_SCGC*` + `AIPS0_PACRN`). Retires `k64uart` and half of `k64dspi`. The ESP32-C6 needs
nothing here: its APM open is a boot-time chip act in `arch_init`, not a per-block request.

**Stage 4 -- the app story.** `kos_cap_narrow(cap, mask)` (rights &= mask, never widen), and
drop or narrow the authority cap in `kickos_default_init_run` before `kickos_app_main`. Plus:
declare **`stress`** privileged-root, since it spawns privileged children (three sites in
`user/apps/common/stress/main.cc`) and a flipped board cannot create a privileged thread after
boot.

`selftest` was on that list and **came off it**. Its only two privileged spawns were in
`rr_interleave`, and they turned out not to be a requirement at all -- the test wants two
threads interleaving under round-robin, which privilege has nothing to do with -- so they were
made unprivileged rather than pinning the whole suite to a posture. That matters beyond the one
app: selftest is the thing that *witnesses* the flip, so leaving it privileged-root would have
made the boundary untestable on exactly the boards that adopt it. It now runs in both postures
(59/60 flipped, `mpu_privileged_guard` skipping because its subject is the privileged posture).

## 8. The three blockers

**Three service bring-up bodies poke MMIO directly from root.**
`system/driver/mk64f/k64uart/k64uart.cc` (AIPS PACR),
`system/driver/mk64f/k64dspi/k64dspi.cc` (clock gates, pin mux, GPIO, DSPI config), and
`system/driver/xmc4800/xmcssc/xmcssc.cc` (USIC kernel clock, baud, protocol). `xmc4800-relax`,
the enforcement flagship, links one of them.

**From root** is the operative phrase for two of them. On the XMC it is only most of the story:
section 9 measures that three of the registers are refused to an unprivileged thread even inside
the granted window, so that bring-up needs both a move into the driver thread and a kernel seam
for those three.

**`root_entry` read argv from the kernel stack.** Fixed in stage 0. Recorded here because of
*how* it would have failed: an unprivileged root faults on its first statement after the ctor
walk, on every enforcing board, and the sim reproduces none of it.

**`user_writable_ok` had no static-data arm.** Fixed in stage 0. Without it, root's writable set on
the five chips with no MPU backend, and on the sim, would have been "its own stack and nothing
else".

## 9. Limits, including the boards where this does not work

- **The XMC SPI blocker IS hardware, measured.**
  Settled on silicon 2026-07-28 by `user/apps/xmc4800-relax/pvprobe` (XMC4800 Relax,
  `KICKOS_HAVE_MPU=1`, probe target U0C1 `0x4003_0200` so the console channel U0C0 stays intact):
  an unprivileged thread holding the MPU grant for the channel window has its writes to `FDR`
  (`0x010`), `BRG` (`0x014`) and `CCR` (`0x040`) **silently discarded** -- read-back returns the
  pre-write value, with no BusFault and no MemManage. In the same thread, same window, same run, a
  write to `SCTR` (`0x034`, `U,PV`) lands exactly, and the run ends on an ungranted SCU poke that
  does MemManage (`CFSR=0x82`, `MMFAR=0x5000_4648`), so both the grant and enforcement are
  witnessed rather than assumed. Root had written all four registers exactly, in the same channel
  state, moments earlier. The discard therefore tracks the CPU privilege level and RM Table
  18-20's `Write = PV` column, and nothing else.

  Consequences. `xmc_spi0_start`'s FDR/BRG/CCR stores need a privileged executor, so the flip
  needs a kernel configure seam for those three; the KSCFG/SCTR/TCSR/PCR/PSCR/DX0CR/INPR rest of
  the sequence is grant-reachable and needs nothing. Given that seam the `xmcssc` bring-up
  **moves wholesale** into the granted unprivileged driver, and has to, because **a post-flip root
  cannot hold a DEV region at all.** `ARCH_MPU_DEV` is attached to a live region in exactly one
  place, `domain_for` (`kernel/domain/domain.cc`), whose only MMIO-carrying caller is
  `thread_spawn` -- `thread_create`'s in-kernel callers pass `mmio_base == nullptr` -- and
  `KOS_SYS_MEM_SELF_GRANT` hardcodes `ARCH_MPU_R | ARCH_MPU_W`. So root cannot be the seam's
  caller: the driver is the only thread that can hold the window, and a split sequence would leave
  a privileged half configuring a channel its own caller cannot address. Stage 3's
  `arch_periph_enable` does not cover it: that seam is "ungate a clock, drop supervisor-protect",
  the K64F/C6 shape, whereas this is USIC-specific FDR/BRG/CCR programming.
  The failure mode with no seam is the bad kind -- a flipped root would program the baud
  generator, take no fault, and leave the channel clocking at whatever the previous value
  implies.

  This record previously read the measurement as "the bring-up cannot move wholesale". It is the
  opposite, and the correction is worth keeping because it is what settles who calls the seam:
  with no path by which root holds a DEV region, there is exactly one candidate.

  The earlier "contradicted by this repo's own silicon" reading was **invalid inference, not a
  measurement**. It rested on `consoledemo`'s scrambler writing FDR/BRG/CCR from
  `privileged=false` and the UART coming out garbled. But the scrambler also writes `SCTR`,
  `TCSR` and `PCR` (all `U,PV`) and gates the channel clock via `KSCFG`, and any one of those
  alone garbles the UART -- so the garbling never required FDR/BRG/CCR to land. It is now known
  they did not.

  Two details of the old transcription: Table 18-20 marks exactly **three** channel registers
  `Write = PV` with no `U` (FDR, BRG, CCR), so `INPR`'s appearance in the earlier
  FDR/BRG/CCR/INPR list was a slip -- INPR is `U,PV` in the table, and the probe did not test it.
  And the additive legend is the manual's front-matter Table 2 (`U` = unprivileged permitted, `PV`
  = privileged permitted), reference-manual knowledge not citable from anything in this tree.

- **The K64F and C6 exemptions from the privileged-write seam family (below) are readings, not
  measurements.**
  `system/driver/mk64f/k64uart/k64uart.cc` writes `BDH`/`BDL`/`C4` from the unprivileged driver
  and its comment claims that "the write proves the driver can own the divisor". It proves nothing
  of the kind, and says why in the sentence before: the re-derived value equals what the kernel
  already programmed, so a silently-dropped store and a landed one leave the same register
  contents and the same legible wire. This is the same invalid-inference shape retracted just
  above for the XMC scrambler -- an outcome consistent with the write landing is not evidence that
  it landed. Neither K64F nor C6 has been probed with a value that *differs*, so their exemption
  from the privileged-write seam family is a plausible reading of the bus documentation and
  nothing more.
- **Granting a peripheral window grants the peripheral, including its own clock -- but only the
  bus-unprivileged subset of it.** `KSCFG` (`MODEN`, the USIC channel's module clock enable) lives
  at `win_base + off::KSCFG`, i.e. *inside* the window handed to the unprivileged driver, and is
  `U,PV`. So an MMIO grant is not "may use this device": it is "may enable, configure and disable
  this device". This is **orthogonal to root's privilege** -- equally true of any unprivileged
  holder on an unflipped board -- and a property of window granularity, not a regression. A window
  that happens to span two peripherals hands over both.

  The grant is an **upper bound**, not the whole window: where a bus enforces its own per-register
  privilege classification, the holder gets strictly less, and on this chip it learns so
  silently (the FDR/BRG/CCR measurement above). So the two limits compose -- window granularity
  sets what is reachable, the bus decides what is writable -- and neither is visible from the
  grant call.
- **`arch_reserved_blocks` reasons about addresses, and interrupt routing is not an address.**
  A granted USIC channel can re-point its `INPR` at a service-request node the kernel owns. No
  address-based admissibility check can see that, because every store involved is inside the
  window the grant legitimately handed over. Measured on silicon by
  `user/apps/xmc4800-relax/inprstorm`: an unprivileged holder of the U0C1 window reroutes
  `INPR`'s RINP/AINP onto SR0, the kernel console's node, and drives the in-kernel
  `console_tx_isr` at **~37,700 invocations/second**. The honest severity is a **bounded parasitic
  CPU tax, not a denial of service** -- the console kept beating and the board did not wedge. The
  reason is that the USIC receive service request is **edge** (one pulse per received word), so a
  held, uncleared flag does not re-assert the node; the same probe confirmed it by leaving RIF/AIF
  set with the attacker idle, which took zero further ISR entries. The tax therefore lasts exactly
  as long as the attacker runs, and the probe pins the attacker below root's priority so a wedge
  could not have been mistaken for starvation.
- **Two threads holding the same MMIO window is unbounded today, and the guard that looks like it
  prevents that does not.** `kernel/domain/domain.cc` skips the domain *dedup* scan when a spawn
  carries MMIO, so an MMIO grant always takes a fresh domain slot. That is a statement about
  domain identity, not about exclusivity: nothing anywhere refuses a second grant of the same
  window, so N threads can hold one peripheral. **Direction: refuse it at grant admission**, as a
  live-DEV-range-overlap test returning `-KOS_EBUSY`, reusing `ranges_overlap`
  (`kernel/grant/grant.cc`) with its existing semantics unchanged -- including
  adjacency-is-not-overlap, which the `frdmk64f` PIT CH2 grant depends on (the chip's reserved PIT
  entry stops at `0x4003_7120` precisely so that grant can sit flush against it). Deliberately
  **not** a per-module count table: that would be hand-maintained and would drift. Sharp edge the
  design has to answer for: a driver respawn issued before the dying driver's domain reference has
  dropped still sees the window live and gets `-KOS_EBUSY`, so a respawner needs
  join-before-respawn or a documented retry.
- **`bluepill-c8` cannot be witnessed on silicon because no unit exists. `f302nucleo` can.** That
  is the binding difference between the two, and it is hardware absence rather than a technical
  verdict. There is no genuine STM32F103C8 on the bench -- the F103 port was physically run only on
  the now-retired 10 K clone (`reference/boards.md`) -- so `bluepill-c8` is a **build-only** target
  and stays one. `microbit` is the same category: the tree records it as **not a physical target**
  at all, a QEMU armv6m vehicle (`m2-readiness.md`), with the additional property that even a unit
  would witness no ring, its Cortex-M0 implementing no privilege axis (section 2). `f302nucleo` has
  a unit, silicon-validated 2026-07-14, and was unwitnessed only for want of being plugged in; it
  is now a bench board (`reference/boards.md`).

  A prior pass re-derived all of this from "neither part has an MPU". That is true and is not the
  binding reason. No MPU rules out the **confinement** arm and nothing else: the **ring** arm
  (section 10) needs a ring and specifically no MPU, and both parts have a real ring -- privilege is
  the fabricated first frame's `ctx.npriv`/`resting_npriv` (section 2), present whether or not an
  MPU is fitted. So `f302nucleo` is the fleet's only physically-present no-MPU ARM board and hence
  the sole possible silicon witness for the ring arm. Nothing has been run on it under an
  unprivileged root: that is future work.

  **`bluepill-c8` costs no coverage.** It is not materially different from `f302nucleo` -- both are
  64 KiB-flash armv7m parts with no MPU and a real privilege ring -- so the Nucleo carries the
  hardware coverage for that whole class, and a second unit would witness the same arm twice.

  The **9-handle provisioning costs the flip nothing**: the authority cap is seated at reserved
  index 2 and spends zero dynamic slots (section 5). Both boards already sit at the suite's floor,
  `MAX_HANDLES 9` and `MAX_THREADS 2` -- root and idle need one slot each -- and per-board table
  sizing recovers zero further bytes on either (`design-flash-footprint.md` section 7), so the
  handle count is a floor rather than a lever.
  The arena is **heap policy, not a property of the part**, measured per board in
  `design-flash-footprint.md` section 7. `bluepill-c8` (20,480 B SRAM, 2 K kernel stack, 8 K
  `.userheap`): **6,560 B** for a production image, **2,592 B** for the selftest image, **14,752 B**
  with the heap carve at zero. `f302nucleo` (16,384 B SRAM, 4 K `.userheap`): **6,464 B** and
  **2,560 B**. The 20 KiB and 16 KiB parts land within 128 bytes of each other because the smaller
  part carries the smaller heap, which is what shows the figure tracks `KICKOS_USER_HEAP_SIZE`.
  The "barely 3 KiB" reading is the selftest-image figure and applies to no production image.
  The two boards are otherwise not one case: `bluepill-c8` carries its own linker script and its
  own `board_config.h` (`boards/bluepill-c8/`), `f302nucleo` has no board directory at all and
  takes the chip's script and the chip's `board_config.h`.
- **No privileged thread can come into existence after boot.** Spawning a privileged child requires
  the caller be privileged, and that is deliberately not a capability. Root is unprivileged and
  `idle` runs no app code, so the posture is one-way for the lifetime of the system. With the knob
  deleted (section 4) this holds on every board rather than on the flipped ones.
- **`idle` stays privileged and holds no capabilities.** It runs no app code, and the
  instruction it exists to execute is not portably available unprivileged. RXv3 is categorical:
  `WAIT` is a privileged instruction and executing it in user mode raises a privileged
  instruction exception (RXv3 ISA UM section 1.4.3). RISC-V is weaker -- `WFI` is "optionally
  available to U-mode" and `mstatus.TW` decides whether it traps, so with no S-mode and `TW`=0 a
  U-mode WFI is permitted, and a conforming implementation may also make it a plain NOP (priv.
  spec sections 3.3.3 and 3.1.6.6). The portable claim is that idle cannot *rely* on it, not that every ISA
  forbids it; Book ch.7.5 carries the long form.
- **The reserved capability index range is full after this** -- 0 stdout, 1 clock, 2 authority,
  3 spare. Spending index 3 would mean the next well-known capability has to raise
  `KICKOS_CAP_FIRST_DYNAMIC`, and *that* costs a dynamic slot on all four 9-handle boards.
- **Delegation packing collides with the reserved names.** Spawn delegation puts cap *i* at
  child index *i+1*, so a delegated cap lands on index 1 (`KOS_CAP_CLOCK`) and a second on index
  2 (the authority slot). Two things follow: the authority cap is non-delegable (section 5), so
  it cannot be the cap that collides; and a spawn asking for both an authority seat and
  `cap_count >= 2` is **refused** rather than letting one silently overwrite the other. The real
  fix is explicit per-grant destination indices, which is deferred.
- **Cap-gen is a `uint16_t`** with no object generation behind a poolless cap, so 65536
  close/re-seat cycles wrap it. Unreachable in-tree, and the same unbounded-counter class as the
  domain-refcount item in `TODO.md`.

### The privileged-write seam family

The shape the design settles on: **as much of a driver in userspace as possible, with a kernel
seam carrying only what the silicon refuses.** The seam is not a convenience layer and does not
grow by argument. Two properties come with that, and they are what the shape buys. The seam's
*size* on a given chip measures that chip's hostility to unprivileged driving, which is a number
rather than an opinion. And the per-chip escalation surface is **enumerable** -- the registers
only the kernel will touch are a list, so they can be read, counted and argued with.

**Two distinct reasons a register lands in the kernel, kept as separate concerns.** They have
different remedies and conflating them loses that:

1. **It is outside any grantable window.** Shared blocks -- SCU, SIM, RCC, AIPS, APM -- carry
   authority over peripherals the caller was never granted, so no window drawn around them is
   admissible at any granularity. `arch_periph_enable` (stage 3) is the seam for this class, and
   the K64F PACR case is its type specimen.
2. **It is inside the window but the bus privilege-gates it.** The grant is admissible, the
   holder can address the register, and the store is still dropped. The measured XMC
   `FDR`/`BRG`/`CCR` case is the only confirmed member.

**Membership requires a measured hardware refusal, never a plausible reading of a manual.**
`user/apps/xmc4800-relax/pvprobe` is the artifact template, and what makes it evidence rather
than a symptom is that it carries **both controls in the same run**: a positive one (`SCTR`, in
the same window from the same thread, lands exactly) and a negative one (an ungranted SCU poke
MemManages). Without the positive control a dropped write is indistinguishable from a broken
grant; without the negative, from unenforced memory. A register with no such measurement behind
it is not in the family, however confident the reading -- which is why the K64F and C6 exemptions
above are recorded as unsettled rather than as findings.

Stated plainly, because pretending otherwise would be worse: this is an **ioctl-shaped** family,
and it is chip-dependent. What bounds it is not elegance but reach -- the seam's reachable command
space is exactly the peripherals its caller has been granted, so a holder of one window can only
ever ask the kernel to configure that window's device.

### The reboot capability

`kos_reboot` is built, as `KOS_SYS_REBOOT` behind the `arch_reboot` seam. It is specifically
**reboot into the chip's bootloader** -- flashing mode (RP2040 `_reset_to_usb_boot`; RP2350
bootrom `reboot` with `REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL`; imxrt1062 the `bkpt` the Teensy's MKL02
catches) -- which is why it sits behind `KICKOS_ENABLE_SELFTEST`: it is a developer affordance for
reflashing without touching the board, not a general system reset. A general reset is not designed
anywhere today and would need its own argument. A chip with no bootloader entry declines through
the seam's weak `-KOS_ENOSYS` default rather than pretending.

**Decision: it folds into `AUTH_DEVICE`**, rather than taking a `CAP_REBOOT` at index 3 or a
sixth rights bit. Three reasons: `arch_shutdown` already sits under `AUTH_DEVICE` and
reboot-to-bootloader is the same class of act (end this system, hand the chip to something
else); there is no sixth bit without merging two existing ones; and index 3 is the last free
well-known index, worth more than bit granularity here.

The counter-argument, recorded because it is real: shutdown merely stops execution, whereas
reboot-to-bootloader leaves the board accepting new firmware over USB. Fusing them means
anything permitted to publish a console can also put the board into flashing mode. That is
acceptable *for a feature compiled out of production images*, and the `AUTH_PINMUX` +
`AUTH_CLOCK` merge stays available if a distinct `AUTH_REBOOT` is ever wanted.

## 10. Verification: what witnesses what, and what nothing witnesses

**The sim is the weakest witness for this work, not the strongest.** It is blind to
kernel-stack faults -- `kmain`'s frame is host stack, so the argv bug could not have been
reproduced there at all -- and it skips non-arena regions. **It also has no privilege axis to
witness at all** (section 2): its `arch_context_init` discards the flag, so "unprivileged" on
the sim means a narrower region set and nothing more. A gate that fires there exercises the
kernel's authority logic, never a CPU-mode boundary. Any claim about the privilege boundary that
rests only on the sim is weaker than it looks. **`microbit` is in the same position for a different
reason** (section 2): its Cortex-M0 implements no privilege axis, so a thread the kernel marks
unprivileged runs privileged there and the armv6m run gate witnesses no CPU-mode boundary either.

**Stage 0 and 1 are verified on sim + QEMU only, and each carries a gate that was checked to
fail.** That last part is the discipline that matters: a gate proving a guard *exists* is worth
much less than one shown to fire.

- `shutdown_priv` -- with the privilege check removed, the run terminates mid-suite with a clean
  exit status and no `# all tests passed`, so the harness fails on a truncated TAP stream.
- `writable_global` -- failed on exactly the two broken postures before the fix (the sim, and
  qemu armv7m with the MPU off) and passed on the enforcing one.
- `authority_cap` -- with the grant removed, the unprivileged child is refused at the pinmux
  gate and the assertion fails. This is the one that covers the *non-privileged* arm of the
  gates, which would otherwise ship unexercised until a board flips -- the same vacuity trap
  `kernel_ctor_placement` fell into.

### The gate has arms, and each arm needs the hardware that makes it mean something

**Capability tracks hardware, exactly as it already does for the MPU.** `KICKOS_HAVE_MPU=1` on a
chip that ships no `mpu.cmake` is a configure `FATAL_ERROR` saying enforcement would be a silent
no-op (the `KICKOS_HAVE_MPU AND NOT KICKOS_CHIP_ENFORCES_MPU` guard, `CMakeLists.txt:285`).
Privilege takes the same treatment: no ring means no privilege enforcement, the kernel's authority
checks still apply, this record states per board what the hardware does, and **nothing marks the
boards that cannot enforce** -- no tier, no status field, no knob.

| Arm | What it witnesses | Hardware it needs |
|---|---|---|
| authority | a capability gate refusing a thread that lacks the bit | none -- pure kernel logic, so the sim and the LX6 included |
| confinement | a cross-domain fault proving memory confinement | an MPU |
| ring | an unprivileged thread refused a privileged-only register | a privilege ring, and no MPU |

The authority arm is what `authority_cap` above witnesses. The confinement arm is selftest green
under enforcement plus a clean cross-domain `rootfault`, which is what the five stage-2 boards
captured and what the sim can never satisfy. The ring arm is the one this record had no instrument
for at all.

**The authority arm never implies confinement, and must not be written as if it did.** On a board
with no ring, an unprivileged root is real in the kernel's authority checks and absent in the
hardware. Nothing there stops a thread walking past the syscall and touching the peripheral
directly. So an authority-arm pass says the kernel refused to act on the machine on that thread's
behalf, and says nothing about what the thread can reach by itself.

**The confinement arm's ordering is why `f411disco` was witnessed in two separate passes**: its
PMSAv7 enforcement had only ever been build-and-link validated, and the arm has no meaning on a
board where enforcement has never been seen to work, so the enforcement witness (selftest 62/62 +
`mpu_fault`, default posture) came first and the flip second. Both are in `reference/boards.md`.

**The ring arm is witnessable and cheap, and it is measured under emulation.** On a synthetic
no-MPU unprivileged-root build under QEMU, an unprivileged access to the System Control Space took
`CFSR=0x8200` -- BFSR byte `0x82`, with the **MMFSR byte `0x00`**, so provably not an MPU fault --
and `BFAR=0xE000E280`, which is `NVIC_ICPR0`, inside the SCS. It escalates to HardFault because
nothing in this tree ever sets `SHCSR.BUSFAULTENA`; only `MEMFAULTENA` is ever written
(`arch/arm/common/arch_arm_common.cc:203` and `:268`, `arch/arm/common/arch_arm_pmsav8.cc:99`).

A prober for it reuses `mpu_fault`'s shape -- root spawns an unprivileged child that pokes the
register, so the app is posture-independent the same way `mpu_fault` is -- with
`tests/check_fault_dump.sh` used **verbatim**: it already asserts the reporter's marker exactly
once plus exit 132, which is how `fault` is wired on every MPS2 image with the `HARD FAULT` marker
(`user/apps/common/fault/CMakeLists.txt:26-29`). The register must be `SHCSR`, `ICSR` or `VTOR`
and **not `STIR`**, because `CCR.USERSETMPEND` can legitimately make `STIR` unprivileged-writable,
and this tree writes `STIR` from the kernel already (`kernel/bench/bench.cc:83`) -- a `STIR` store
that lands would prove nothing about the ring.

**Deleting the knob turns a never-built gate into an always-built one, which argues for the
deletion on its own.** `user/apps/common/rootfault/CMakeLists.txt:18` registers the confinement
gate under `KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND NOT KICKOS_ROOT_PRIVILEGED`, and **no
preset and no CI job in the tree configures that posture** -- every enforcement build passes
`-DKICKOS_HAVE_MPU=1` and leaves the knob at its default ON. That is exactly why the confinement
arm has only ever been witnessed by hand on silicon. With the flag gone the third condition is
vacuous and the gate becomes unconditional wherever enforcement exists: the four MPS2 images
(`qemu`, `qemu-m3` and `qemu-m7` on PMSAv7 -- the M7 image with 16 regions -- and `qemu-m33` on
PMSAv8) plus `qemu-riscv` on RISC-V PMP.

**What nothing witnesses.** Two coverage gaps here are structurally unfillable by emulation:
v6-M MPU programming (QEMU models no Cortex-M0+ and no Cortex-M23 core) and the M7 speculation
class. Neither can be closed by adding a gate; both stay silicon-proven or unproven.
