<!-- SPDX-License-Identifier: CECILL-C -->
# Unprivileged root -- start unprivileged holding capabilities

> **Status: ACTIVE** -- stages 0 and 1 have landed on the M4.5.1 branch, and **stage 2 has landed
> for its first board**: `xmc4800-relax` boots an unprivileged root and is witnessed on silicon
> (selftest green under PMSAv7 enforcement plus a clean cross-domain `apps/rootfault`; see
> `reference/boards.md`). The remaining boards in the flip order, and stages 3-4, are still
> planned and hardware-gated. See `design/README.md` for the marker taxonomy. The actionable
> checklist is `TODO.md`; this record is the reasoning behind it, which `TODO.md` does not carry.

Root is the kernel's first application thread: `kmain` creates it, it runs the app and
library constructors, then calls `kickos_init_entry`. It **was** privileged for its entire
life, holding every authority the kernel can grant for that whole time -- and on a board that
has not flipped (`KICKOS_ROOT_PRIVILEGED` defaults ON) it still is. That default is what this
record argues should end, board by board.

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
| armv6m | the same two fields (`arch/arm/armv6m/arch_armv6m.cc`), read by `switch.S` at fixed offsets 4 and 8 |
| rv32imac | `mstatus.MPP` in the fabricated frame -- M-mode for privileged, U (0) otherwise (`arch/riscv/rv32imac/arch_rv32imac.cc`) |
| rxv3 | `PSW_THREAD_USER` in the fabricated PSW (`arch/rx/rxv3/arch_rxv3.cc`) |
| lx6 (Xtensa) | nothing to set: the LX6 has no ring split, so the flip is a no-op here |
| sim | nothing set and nothing recorded: `arch_context_init` takes `privileged` and discards it (`arch/sim/sim.cc`) |

Three consequences follow.

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
bring-up bodies in section 8 want. It is in scope only if the `arch_periph_enable` seam (stage
3) proves insufficient for them.

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

Measuring that last one changed the plan: the hole is **not** confined to the five no-MPU chips
(stm32f103, stm32f302, nrf51, sam3x8e, esp32). **The host sim has it too**, despite building
`KICKOS_HAVE_MPU=1`, because its globals live in the host image rather than the mprotect'd
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
that is right on the day it is written is exactly what lets the next one hide.

The reads of `Thread::privileged` that survive are exactly the set this design keeps:

- the privileged arm inside `cap_check_authority` -- "privileged implies every authority",
  stated once;
- **spawn-a-privileged-child**, which is deliberately *not* a capability, because holding it is
  equivalent to holding everything forever;
- the child-privilege selection of memory posture in `thread_spawn`;
- the confused-deputy bypass in `syscall_mem.cc`.

**Stage 2 -- flip per board**, behind a build-enforced `KICKOS_ROOT_PRIVILEGED` knob, default
ON. **Not a weak symbol**: opting out of the boundary must be visible in the board's build, not
silently satisfied by a link-time override. Order: `xmc4800-relax` with a console-only service
list first (its `xmcuart` bring-up is pure syscall), then f411disco, frdmk64f, pizero2350,
esp32c6-wroom, rx72m.

**Stage 3 -- `arch_periph_enable(base)`**, weak `-KOS_ENOSYS`, gated `AUTH_DEVICE`, covering
"ungate the clock and drop supervisor-protect for the block at this base". Implemented for K64F
(`SIM_SCGC*` + `AIPS0_PACRN`) and ESP32-C6 (the APM/PMS one-time open). Retires `k64uart` and
half of `k64dspi`.

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
the enforcement flagship and the first board in the flip order, links one of them.

**From root** is the operative phrase, and section 9 revises what that costs on the XMC: the
registers sit in a window the driver already grants to an unprivileged thread, so the blocker is
where the bring-up runs, not what the silicon permits.

**`root_entry` read argv from the kernel stack.** Fixed in stage 0. Recorded here because of
*how* it would have failed: an unprivileged root faults on its first statement after the ctor
walk, on every enforcing board, and the sim reproduces none of it.

**`user_writable_ok` had no no-MPU arm.** Fixed in stage 0. Without it, root's writable set on
five chips would have been "its own stack and nothing else".

## 9. Limits, including the boards where this does not work

- **The XMC SPI blocker is software PLACEMENT, not hardware.** This bullet used to say the
  PV-write-only registers were "privileged by hardware design", which framed the blocker as a
  property of the silicon. Checked against the driver, that is not what stops the flip.

  `xmcssc` already splits itself: `xmc_spi0_start` does a privileged one-time bring-up, then
  `spawn_unprivileged(xmcssc_service, win_base, win_size, ...)` hands the U0C1 window
  (`0x4003_0200`) to an **unprivileged** driver thread which does every transfer inside it. So
  the window is not ungrantable -- **this driver already grants it.** What breaks under the flip
  is that the bring-up runs in *root*, and a flipped root holds no MMIO grant for U0C1. Move
  that sequence to something that holds the grant and the obstacle goes with it. Stage 3's
  `arch_periph_enable` genuinely does not cover this -- it is "ungate a clock, drop
  supervisor-protect", the K64F/C6 shape, whereas the XMC needs USIC-specific KSCFG/FDR/BRG/CCR
  programming -- but "stage 3 does not cover it" is a different claim from "the hardware
  forbids it", and only the first one is supported.

  **The PV-write-only reading is contradicted, not untested.** That FDR/BRG/CCR/INPR reject an
  unprivileged write comes from RM Table 18-20 as transcribed into the driver banner. This repo's
  own silicon record refutes it, and the two were never put side by side:
  `user/apps/xmc4800-relax/consoledemo/main.cc` spawns its scrambler `privileged=false` holding
  the granted U0C0 window and writes exactly FDR, BRG, SCTR, TCSR, PCR, CCR and `KSCFG` from
  inside it, and the recorded XMC silicon PASS is that the panic banner survives a
  *driver-garbled* UART -- which it can only be if those unprivileged writes landed. So the
  bring-up can move wholesale into the granted unprivileged driver, and needs no seam: the
  register-level obstacle the reading described is not there.

  What remains genuinely unread is **RM Table 18-20 itself** -- the banner transcribes a reading
  of it and nobody has gone back to the table -- and the `U`/`PV`/`BE` privilege glossary is
  reference-manual knowledge, not citable from anything in this tree.

  The flip order still starts with `xmc4800-relax` because its *console* bring-up is pure
  syscall -- that part was right.
- **Granting a peripheral window grants the peripheral, including its own clock.** `KSCFG`
  (`MODEN`, the USIC channel's module clock enable) lives at `win_base + off::KSCFG`, i.e.
  *inside* the window handed to the unprivileged driver. So an MMIO grant is not "may use this
  device": it is "may enable, configure and disable this device". This is **orthogonal to root's
  privilege** -- it is equally true of any unprivileged holder on an unflipped board -- and it is
  a property of window granularity, not a regression. Recorded because the natural reading of
  "unprivileged driver" is more confined than what is actually handed over, and because a window
  that happens to span two peripherals would hand over both.
- **`bluepill-c8` and `f302nucleo` will likely never flip.** Both carve barely 3 KiB of arena
  for the two boot stacks, and both are 9-handle boards.
- **On a flipped board, no privileged thread can come into existence after boot.** Spawning a
  privileged child requires the caller be privileged, and that is deliberately not a
  capability. So the flip is one-way for the lifetime of the system.
- **`idle` stays privileged and holds no capabilities.** It runs no app code, and the
  instruction it exists to execute is not portably available unprivileged. RXv3 is categorical:
  `WAIT` is a privileged instruction and executing it in user mode raises a privileged
  instruction exception (RXv3 ISA UM §1.4.3). RISC-V is weaker -- `WFI` is "optionally available
  to U-mode" and `mstatus.TW` decides whether it traps, so with no S-mode and `TW`=0 a U-mode
  WFI is permitted, and a conforming implementation may also make it a plain NOP (priv. spec
  §3.3.3, §3.1.6.6). The portable claim is that idle cannot *rely* on it, not that every ISA
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

### The reboot capability

`kos_reboot` is designed but unbuilt. It is specifically **reboot into the chip's bootloader**
-- flashing mode (RP2040 `reset_usb_boot`; RP2350 bootrom `reboot` with
`REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL`) -- which is why it sits behind `KICKOS_ENABLE_SELFTEST`: it
is a developer affordance for reflashing without touching the board, not a general system reset.
A general reset is not designed anywhere today and would need its own argument.

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
rests only on the sim is weaker than it looks.

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

**Stage 2's per-board gate** is selftest green under enforcement **plus** a clean cross-domain
`mpu_fault` proving root is confined. The first witness must be an **enforcing ARM board**, not
the sim.

**What nothing witnesses.** Two coverage gaps here are structurally unfillable by emulation:
v6-M MPU programming (QEMU models no Cortex-M0+ and no Cortex-M23 core) and the M7 speculation
class. Neither can be closed by adding a gate; both stay silicon-proven or unproven.
