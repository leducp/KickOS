<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Unprivileged root: start unprivileged holding capabilities

> **Status: LANDED.** Why root starts unprivileged holding capabilities instead of starting
> privileged and demoting, plus the boards where that does not work. All five stages merged.

Decisions only. The contract is `reference/invariants.md` and `reference/porting.md`; captures are
`reference/boards.md`; footprint numbers are `archive/M4.5_footprint_meas.md` section 7. Section
numbers are cited from source and CMake files, so no heading here is renumbered or removed.

## 1. The decision, and the design it superseded

**REJECTED: a drop-privilege syscall** (boot privileged, bring up, demote before `main`). It needs a
per-arch backend to change a *running* thread's privilege, needs `thread_regions_recompose` to rebuild
the region set at the demotion instant, and puts Xtensa last for having no ring to demote across.

**TAKEN: root is created unprivileged, holding capabilities for the authorities its bring-up needs.**
A transition can be skipped, forgotten on one port, or raced; a property of the fabricated first frame
is decided once at `thread_create` and never revisited.

## 2. Why it needs no new assembly on any port

Every ISA with a ring split already encodes privilege in the fabricated first frame and restores it on
the first switch-in. This table is the per-board ring classification a new armv6m board must state in
`user/apps/common/ringpriv/CMakeLists.txt`.

| Port | Where privilege lives in the fabricated frame |
|---|---|
| armv7m | `ctx.npriv` and `ctx.resting_npriv` (`arch/arm/armv7m/arch_armv7m.cc`) |
| armv6m, Cortex-M0+ (`picopi`) | the same two fields (`arch/arm/armv6m/arch_armv6m.cc`), read by `switch.S` at fixed offsets 4 and 8 |
| armv6m, Cortex-M0 (`microbit`) | written, and the hardware discards them: ARMv6-M's Unprivileged/Privileged Extension is optional, separate from the MPU extension, and the M0 does not implement it (Cortex-M0 TRM DDI0432C) |
| rv32imac | `mstatus.MPP`, M-mode privileged and U otherwise (`arch/riscv/rv32imac/arch_rv32imac.cc`) |
| rxv3 | `PSW_THREAD_USER` in the fabricated PSW (`arch/rx/rxv3/arch_rxv3.cc`) |
| lx6 (Xtensa) | nothing to set: the LX6 has no ring split |
| sim | `arch_context_init` takes `privileged` and discards it (`arch/sim/sim.cc`) |

- **On `microbit` the kernel's privilege bit is a fiction.** One arch backend spans a core with a ring
  and one without, so the classification cannot be per arch.
- **Xtensa comes along free instead of last**: with no ring split an "unprivileged" thread is the same
  thread, and the capability gates still narrow what it may ask the kernel to do.
- **`resting_npriv` is the tell.** Privilege is a *resting* property the syscall path leaves and
  returns to, so changing it mid-life means updating the resting value too, in asm, on every port.
- **The sim has no privilege axis**, so it cannot tell a privileged thread from an unprivileged one
  holding a permissive region set. Section 10 is where that bill comes due.

## 3. Why the region set needs no recomposition

`thread_create` composes the set once from a privilege that never changes, so
`thread_regions_recompose`, which existed only to rebuild it at the demotion instant, has no reason to
exist. That is why the change is mostly deletion.

## 4. What was deleted rather than deferred

`thread_regions_recompose`, the drop-privilege syscall, its per-arch backends and the Xtensa-last
sequencing are **deleted, not deferred**: carrying transition machinery as "later" implies it is still
wanted. `drop_priv` survives as a contingent, much smaller item, in scope only if the privileged-write
seam family (section 9) proved insufficient for the blocked bring-up bodies. It did not.

### The root-privilege knob went too, with no replacement

- **REJECTED: keeping the knob in any form** (bypass, porting helper, tier, or a consumer-refusing
  tombstone guard, which is not in the tree). A knob is a posture someone can ship by accident, and
  this one's posture was "root holds every authority for the life of the system".
- **REJECTED: silently ignoring it instead of deleting it**, since a downstream build still setting it
  would go on believing it had a posture to choose. Accepted residual of that class: the scramble-test
  option was also deleted with no tombstone, so an old configure switching it on silently builds a
  plain `consoledemo` with nothing naming the `conreclaim` app that replaced it.
- **The safe default flipped with it**, `ThreadAttr::privileged` having been `true`, so a struct that
  forgot the field minted a fully-authorized thread. The banner's `", root unprivileged"` suffix went
  for the knob's own reason: under one posture it was a constant.
- **The root-MMIO service-list refusal was re-subjected from posture to enforcement**
  (`KICKOS_HAVE_MPU`, `CMakeLists.txt`), and the change of subject is the decision. Measured, not
  tidy-minded: with the MPU off an unprivileged root **does** reach MMIO, so refusing an unenforcing
  build would refuse a configuration that demonstrably works. The root-MMIO list was EMPTIED
  here, and on 2026-08-05 the list and its gate were DELETED outright: a refusal that cannot fire on
  any configure in the tree is not a guard.
- **`root-unprivileged-idle-alone-privileged` became statable**, a build under the knob having always
  been able to answer "two, and one of them is root". Contract and consequences live in
  `reference/invariants.md`, not here.

## 5. The authority capability: shape, seat, and the arithmetic behind it

Gates that read `privileged` call `cap_check_authority(caller, AUTH_*)` and nothing else, with the
privileged-implies-everything arm *inside* that function so no call site can encode it differently.

> **Superseded in mechanism, not in cut.** Authority is no longer a capability. The word lives in the
> TCB as `Thread::authority`, a byte in padding the struct already carried, because it named no pool
> object, held no refcount and bumped no generation -- so the table was charging an index on every
> board and returning none of its properties. `KOS_CAP_AUTHORITY` survives only as the pseudo-handle
> `kos_cap_narrow` takes to name the word. The gate chokepoint above, the bit cut in 5.1, the
> narrowing rule in 5.2 and the revocation argument in section 6 are all unaffected; what follows is
> the reserved-index design as it was decided here. Live contract:
> `reference/invariants.md` (`authority-word-narrows-only`).

- **It was poolless**, the capability being its authority bits, so it resolved by reading its reserved
  slot and never through `cap_resolve_e` (which would read that word as a pool index), and the refcount
  arms were explicit no-ops so a missing arm traps rather than leaking silently.
- **The authority word lived in `obj`, and that is what funded a six-bit set.** `CapEntry.rights` had
  five bits free, one short of 5.1's cut, and `CapEntry` is a frozen 8-byte ABI. This retired the "merge
  `AUTH_PINMUX` with the clock-rate bit" plan the old ceiling forced. Accepted price: an object right is
  no longer a *distinguishable* wrong value in a mask, so the non-authority-bits spawn refusal catches
  only bits above the six. (A dedicated TCB byte keeps the six-bit set and the accepted price alike,
  and buys two more bits before anything has to widen.)
- **The `obj` move had one blocker, closed first.** The delegation copy in `thread_spawn` copies `obj`
  and `type` with no type test, so with the word in `obj` a delegable authority cap becomes a full
  forgery in a child table. That copy took an explicit refusal of the authority cap TYPE **first**,
  rather than resting on rights, so it did not depend on the byte this change repurposes. (Moving the
  word off the table retires the blocker instead of guarding it: with no entry there is nothing for
  the copy to copy, and the refusal went with the type.)
- **Seated at the already-reserved index 2**, so it cost zero dynamic slots on every board including
  the four smallest. That was the whole reason it was a rights-bearing cap at a reserved index rather
  than a new object. (The TCB byte costs no index at all, which is strictly better on the same
  arithmetic, and freed index 2 back to the dynamic range.)
- **REJECTED: per-instance capabilities, on arithmetic rather than taste.** One capability per muxable
  pin, clock or line is roughly 100 on the larger parts against a 16-slot table ceiling. It does not
  nearly fit, so class granularity is forced.

### 5.1 The authority set, re-cut into six

**REJECTED: one combined device bit** meaning "console publish, shutdown, reboot", which holds together
only while root does all three. Once root is only a spawner those belong to different threads, and one
bit hands the console driver the power to end the system. `cpu_clock_set` keeps a bit of its own for
the same reason.

| Bit | Gates | Who holds it |
|---|---|---|
| `AUTH_MEMORY` | `ram_alloc`, the MMIO window grant, `mem_self_grant` | root, the spawner |
| `AUTH_PINMUX` | `pinmux_set` | root, for the board pin map; an app muxing its own pins |
| `AUTH_PSTATE` | `cpu_clock_set` | a CPU-governor service, and nothing else |
| `AUTH_IRQ` | `irq_attach`, `irq_unmask` | drivers with lines |
| `AUTH_SYSTEM` | `shutdown`, `reboot` | root / init |
| `AUTH_CONSOLE` | `console_publish` | root, during service bring-up |

- **The console driver does not hold `AUTH_CONSOLE`, and that is not a future correction to make.**
  Root publishes inside the service list's `start()` body and the unprivileged child only *receives*,
  so root holds the bit for the length of bring-up and drops it before `main`, keeping `AUTH_SYSTEM`
  for the shutdown that ends a returning app. One bit could not express that.
- **`AUTH_CONSOLE` cannot be possession-gated** the way `arch_periph_enable` is:
  `KOS_SYS_ENDPOINT_CREATE` is ungated so any thread can mint the endpoint it would publish, and
  `cap_console_publish` has no owner check and no once-only guard, so a publish silently steals a live
  console. Those two missing checks are wanted whichever bit gates the call (`TODO.md`).

### 5.2 Dropping an authority: `kos_cap_narrow`

- **It is ungated, and it has to be.** An authority required in order to drop authorities is one no
  thread could ever give up, and it clears bits only in the caller's own authority word. Narrowing to
  0 leaves nothing further to give up, so a second narrow is refused `-KOS_EBADF`.
- **It refuses any handle that does not name the authority word** (`-KOS_EINVAL`). Narrowing an object
  cap is not merely unimplemented: dropping `CAP_WAIT` from an endpoint cap must run the `recv_holders`
  accounting close performs, or the last-receiver `EPIPE` wake goes wrong. Keeping the handle argument
  is what leaves that generalisation ABI-free.
- **REJECTED: declaring the narrow mask as a CMake variable**, one value per build tree while one tree
  links `selftest` and `stress` against one kernel. It is per executable, `KICKOS_APP_AUTHORITY` in the
  app's own TU. The narrow runs after the pin map and the service list, so bring-up still holds its
  bits when it needs them.
- **An app whose `main` returns must keep `AUTH_SYSTEM`**, and the bit is deliberately not forced back
  on: a refused `kos_shutdown` panics legibly, while forcing it would deny a never-returning app the
  ability to declare 0.
- **The seat is unconditional, so the narrow always has something to narrow**, which is what confines
  an app everywhere rather than only where a knob was flipped. The privileged short-circuit inside
  `cap_check_authority` survives, but only `idle` can take it, and `idle` runs no app code.

## 6. The actual win: revocation

Confining root's *memory* is not the main prize. The prize is that an authority can be **dropped**: root
pin-muxes the board, drops `AUTH_PINMUX` before `main`, and the kernel then refuses a pinmux request
from it including one made by application code that has gone wrong. Without `kos_cap_narrow` this would
be a memory-isolation change and nothing more.

## 7. Stage plan, and what each of the five stages landed

The five-stage sequence is git history and `STATE.md`. **Stage 3** is `arch_periph_enable`
(`KOS_SYS_PERIPH_ENABLE = 39`), **stage 4** is `kos_cap_narrow` (`KOS_SYS_CAP_NARROW = 40`) with the
six-bit re-cut and the `obj` move (section 5), **stage 5** is the knob deletion (section 4); both
seams' contract is `privileged-write-seam-possession-and-allowlist`. What lives nowhere else:

- **REJECTED: gating `arch_periph_enable` on an authority bit.** The gate is **possession**. A bit
  fails three ways: its callers are bus drivers, so the lifecycle bit hands `kos_shutdown` and
  reboot-to-bootloader to every unprivileged driver in the fleet; every other existing bit carries
  collateral just as unwanted; and a bit of its own faces the same possession argument that removes the
  need for one. Possession is *sufficient* because a granted window already means "may enable,
  configure and disable this device" wherever the silicon permits it directly (section 9).
- **REJECTED: `user_range_ok` as that predicate**, which asks whether the kernel may dereference a
  user pointer and passes trivially at `len == 0`. Exact base rather than containment is what stops a
  sub-block window reaching a whole-block table entry.
- **REJECTED: repairing `mpu_privileged_guard` instead of deleting it.** With the knob gone its
  registration and skip conditions were both exactly `KICKOS_HAVE_MPU`, so it would have skipped in
  100% of the builds in which it existed. Unrepairable too: it needs a privileged thread running a
  test, and the only privileged thread left is `idle`. `rootfault` replaces it with the stronger claim,
  "root is confined" rather than "a privileged thread is exempt from confinement".
- **Falsifier: enumerate the authority DECISIONS, never count the call sites.** The conversion counted
  eight and missed a ninth, `grant_region_admissible`'s DEV arm, still reading raw
  `Thread::privileged`; invisible while root is privileged, and on the console-handover path, so the
  board went dark. The enumeration that stays true is the grep, every gate being a
  `cap_check_authority` call site. The `Thread::privileged` reads this design keeps: that privileged
  arm; spawn-a-privileged-child, deliberately not a capability because holding it equals holding
  everything forever; the child-privilege memory-posture selection in `thread_spawn`; the
  confused-deputy bypass in `syscall_mem.cc`.

## 8. The four blockers

All four are retired and none is pending: three service bring-up bodies moved into the driver thread
that holds the window, and the argv and `user_writable_ok` bugs were fixed before any board flipped.
Two records outlive them:

- **The argv bug's failure shape, which the sim cannot reproduce.** Reading `argc`/`argv` from a `kmain`
  frame local faults on root's first statement on every enforcing board, the boot stack being outside the
  arena, while the sim's host-stack frame sees nothing. They live in `kickos_init_args` now, deliberately
  not the init provider's archive, because a build naming its own `KICKOS_INIT_PROVIDER` must not be able
  to take away a definition `kmain` references unconditionally. Worse than the bug: test code had already
  worked around it in a comment, and nobody filed it.
- **The widened panic reclaim gate rests on chip-body idempotence.** The ownership axis records a
  PUBLISH and says nothing about whether the device is GARBLED, so the gate widened to any state. That
  promotes the harmless-idempotent reading of the chip bodies from an aside to the justification, and a
  body that read-modify-writes or assumes a driver configured the channel breaks the widened gate
  (`panic-console-probe-independent`).

## 9. Limits, including the boards where this does not work

- **The XMC SPI blocker IS hardware, measured.** An unprivileged holder of a USIC channel window has
  its writes to `FDR`, `BRG` and `CCR` silently discarded (`user/apps/xmc4800-relax/pvprobe`; wire in
  `reference/boards.md`, the `U`/`PV` derivation in `reference/porting.md`). The consequence that
  decides the design: **root cannot hold a DEV region at all**, so the driver is the only thread that
  can hold the window and the bring-up had to move wholesale rather than split. The no-seam failure is
  the bad kind, a baud generator programmed with no fault at all.
- **Accepted residual: the K64F and C6 exemptions from the seam family are readings, not
  measurements.** `k64uart` writes `BDH`/`BDL`/`C4` from the unprivileged driver, but the re-derived
  value equals what the kernel already programmed, so a dropped store and a landed one leave the same
  register contents and the same legible wire. Neither chip has been probed with a differing value.
- **Granting a peripheral window grants the peripheral, including its own clock, but only the
  bus-unprivileged subset of it.** `KSCFG` sits inside the granted window, so a grant means "may
  enable, configure and disable this device", and a window spanning two peripherals hands over both.
  Orthogonal to root's privilege, and a property of window granularity rather than a regression. The
  grant is an **upper bound**: where a bus enforces per-register privilege the holder gets strictly
  less, learns so silently, and neither limit is visible from the grant call.
- **An `arch_periph_enable` entry exists only where the bus gate's granularity is contained by the
  block the window covers, and the K64F PIT is where that fails.** One AIPS `PACR` slot governs a whole
  4 KiB block, so opening the PIT channel-2 slot would equally expose the chained pair carrying
  `arch_clock_now`. **The refusal is the design working, not an omission**, which separates its
  consequence from `f411spi`'s: `k64drv` cannot run unprivileged *by design* and now has no path at
  all, a decision to take about the app rather than a gap to close (`m2-readiness.md`).
  `arch_periph_reg_write` does not rescue it, the obstacle being granularity and not privilege.
- **The CPU/peripheral clock coupling is over-generalised, and should be a question asked of the chip.
  M4.6 work.** `cpu_clock_set` refuses outright while a userspace driver owns the console, on the
  grounds that the kernel cannot re-derive a baud it no longer owns. That veto generalises from a
  biased sample: five chips implement `arch_periph_clock_hz` and the two this argument rested on are coupled, so the
  decoupled case has never had to be stated. The right shape is a **notification to the affected
  services**, and the console is not the only one.
- **`arch_reserved_blocks` reasons about addresses, and interrupt routing is not an address.** A
  granted USIC channel can re-point `INPR` at a service-request node the kernel owns, invisible to any
  address-based check because every store is inside the legitimately granted window
  (`user/apps/xmc4800-relax/inprstorm`, three-point rate sweep in `reference/boards.md`). Honest
  severity: a **bounded parasitic CPU tax, not a denial of service**. The bound is the SCHEDULING model
  rather than the baud, which matters because the seam hands the attacker the rate knob, so a verdict
  resting on one operating point is one the attacker can reconfigure. Structural: the sustained rate is
  `min(fill, drain)` with `fill` the attacker's sub-root CPU share refilling a finite 64-deep buffer, so
  depth and clock rate trade off rather than multiply.
- **One holder per MMIO window, enforced at grant admission**, matched on RANGES rather than slots so
  an **adjacent** window stays admissible, which the `frdmk64f` PIT CH2 grant depends on. **REJECTED: a
  per-module count table**, hand-maintained and certain to drift. Accepted sharp edge: a respawn issued
  before the dying driver's domain reference has dropped gets `-KOS_EBUSY`, so a respawner needs
  join-before-respawn or a documented retry.
- **`bluepill-c8` cannot be witnessed on silicon because no unit exists. `f302nucleo` can.** That is
  the binding difference, and it is hardware absence rather than a technical verdict. No MPU rules out
  the **confinement** arm and nothing else, the **ring** arm needing a ring and specifically no MPU, and
  both parts have a real ring (section 2), which makes `f302nucleo` the fleet's sole possible silicon
  witness for that arm. `microbit` is a third category, not a physical target at all, and even a unit
  would witness no ring. **`bluepill-c8` costs no coverage**: both are 64 KiB-flash armv7m parts with no
  MPU and a real ring, so the Nucleo carries the class. The arena is **heap policy, not a property of
  the part**: at equal carve the 20 KiB and 16 KiB parts land within 128 bytes of each other, so any
  "barely 3 KiB" reading is a selftest-image figure and applies to no production image.
- **`idle` stays privileged and holds no capabilities**, its one instruction not being portably
  available unprivileged: RXv3 `WAIT` raises a privileged instruction exception in user mode (RXv3 ISA
  UM section 1.4.3), and RISC-V `WFI` is only optionally available to U-mode with `mstatus.TW` deciding
  whether it traps, and may also be a plain NOP (priv. spec sections 3.3.3 and 3.1.6.6). The portable
  claim is that idle cannot *rely* on it. Book ch.7.5 has the long form.
- **The reserved capability index range was full after this**: 0 stdout, 1 clock, 2 authority, 3 spare.
  Spending index 3 meant raising `KICKOS_CAP_FIRST_DYNAMIC`, which costs a dynamic slot on every board
  in the fleet. (Moving the authority word to the TCB gave two of those indices back; the range is 0
  stdout, 1 clock, and the reserved-index arithmetic is the standing reason to prefer TCB state for
  anything that names no object.)
- **Delegation packing collides with the reserved names.** Spawn delegation puts cap *i* at child index
  *i+1*, so a delegated cap lands on `KOS_CAP_CLOCK` and a second on the slot after it. The authority
  seat was non-delegable so it could not be the colliding cap, and a spawn asking for both an authority
  seat and two or more delegated caps was **refused** rather than letting one silently overwrite the
  other. (That refusal is gone with the seat: there is no entry to overwrite. The clock-index collision
  is not -- delegated cap 0 still lands on it.) The real fix, explicit per-grant destination indices,
  is deferred.
- **Cap-gen is a `uint16_t`** with no object generation behind a poolless cap, so 65536 close/re-seat
  cycles wrap it. Unreachable in-tree, same unbounded-counter class as the domain-refcount item in
  `TODO.md`. (Moot for authority, which has no slot and no generation; it still applies to the reply
  cap.)

### The privileged-write seam family

**As much of a driver in userspace as possible, with a kernel seam carrying only what the silicon
refuses.** Not a convenience layer, and it does not grow by argument. The shape buys two properties:
the seam's *size* on a chip measures that chip's hostility to unprivileged driving, and the per-chip
escalation surface is **enumerable**.

- **Two distinct reasons a register lands in the kernel, kept as separate concerns** because the
  remedies differ. (1) **Outside any grantable window**: shared blocks (SCU, SIM, RCC, AIPS, APM) carry
  authority over peripherals the caller was never granted, so no window drawn around them is
  admissible at any granularity; `arch_periph_enable` is the seam. (2) **Inside the window but bus
  privilege-gated**: the grant is admissible, the holder can address the register, the store is still
  dropped; `arch_periph_reg_write` (`KOS_SYS_PERIPH_REG_WRITE = 42`) is the seam, and the measured XMC
  case is its only confirmed member. The two ABI shapes follow from that split rather than from
  convenience (`reference/porting.md`).
- **Falsifier, and it was a real confused deputy: the class-2 justification named a premise the code
  did not establish.** "The register is inside its own window and it already reads that address freely"
  is only true if something CHECKS it, and the possession predicate ignored the region's size, so the
  only bound on the target address was the chip allowlist, which bounds which registers and not which of
  them this caller may reach. Measured, and exploitable on the flagship rather than theoretical. The fix
  restores the premise instead of abandoning the ABI, by also requiring CONTAINMENT
  (`reference/porting.md`).
- **Membership requires a measured hardware refusal, never a plausible reading of a manual.**
  `pvprobe` is the artifact template, and what makes it evidence is both controls in one run: without
  the positive one a dropped write is indistinguishable from a broken grant, without the negative one
  from unenforced memory. **The same standard applies to the seam itself**, so the probe hands the seam
  the very value the direct store had just failed to land. Class 1 has no equivalent:
  `arch_periph_enable`'s positive arm has no probe and is witnessed only by the bring-up it enables.
- Stated plainly, because pretending otherwise would be worse: this is an **ioctl-shaped**,
  chip-dependent family. What bounds it is not elegance but reach, and **the bound holds because the
  call site is the driver rather than root**, a fact about the code and not an aspiration. A seam gated
  on an authority *bit* would carry no such bound, any holder being able to name any base in the table.

### The reboot capability

`kos_reboot` is specifically **reboot into the chip's bootloader**, hence `KICKOS_ENABLE_SELFTEST`: a
developer affordance for reflashing without touching the board, not a general system reset.

**Decision: it shares `arch_shutdown`'s authority, `AUTH_SYSTEM`**, rather than a dedicated capability
type at a well-known index or a rights bit of its own. Reboot-to-bootloader is the same class of act
as shutdown, and the last free well-known index was worth more than bit granularity here (the reserved
range has since shrunk, which only strengthens that). The
counter-argument, recorded because it is real: reboot-to-bootloader leaves the board accepting firmware
over USB, so anything permitted to end the system can also put it into flashing mode. Accepted *for a
feature compiled out of production images*, and the sharper half, a console publisher inheriting
flashing mode, is answered by the re-cut rather than by a merge.

**Decided, not yet applied: `arch_reboot` takes a MODE, and the compile knob gates the mode rather than
the seam.** Owned by M4.6. The `-KOS_ENOSYS` decline becomes per-MODE rather than per-function, since a
chip that can reset but has no documented bootloader entry declines both today; the asymmetry motivating
the split is ARM-specific and architectural rather than measured, `SCB->AIRCR` `SYSRESETREQ` needing no
chip-specific code while bootloader entry is per-chip. The knob then gates only the bootloader mode,
because `KICKOS_ENABLE_SELFTEST` conflates "test-only syscall surface" with "may this image put the
board into firmware-accept mode", costing a real capability for no security gain: a privileged thread
may need a watchdog recovery or a fault-handler reset, neither carrying the bootloader's risk.
**REJECTED: an `..._IN_FLASH_MODE` spelling**, the sibling knob being `KICKOS_SHUTDOWN_TO_BOOTLOADER`
and two knobs naming one destination differently being drift. Bootloader mode wants the knob AND the
authority bit, belt-and-braces being proportionate for that one act and not the other. Two debts retire
with it: `KICKOS_SHUTDOWN_TO_BOOTLOADER` becomes a POLICY on one seam instead of a parallel mechanism,
and syscall 38 becomes a real production syscall, retiring its configure-time `FATAL_ERROR` and the
`abi.h` annotation that documents the compiled-out arm.

## 10. Verification: what witnesses what, and what nothing witnesses

**The sim is the weakest witness for this work, not the strongest.** It is blind to kernel-stack
faults, it skips non-arena regions, and it has **no privilege axis at all** (section 2), so a gate
firing there exercises the kernel's authority logic and never a CPU-mode boundary. `microbit` is in the
same position for a different reason. The distinction matters because the sim arm is a CI gate whose
pass is easy to over-read.

| Arm | What it witnesses | Hardware it needs |
|---|---|---|
| authority | a capability gate refusing a thread that lacks the bit | none, pure kernel logic, so the sim and the LX6 included |
| confinement | a cross-domain fault proving memory confinement | an MPU |
| ring | an unprivileged thread refused a privileged-only register | a privilege ring, and no MPU |

- **The authority arm never implies confinement, and must not be written as if it did.** With no ring
  an unprivileged root is real in the kernel's authority checks and absent in the hardware, and nothing
  stops a thread walking past the syscall to the peripheral. Corollary: the arm is witnessable where
  the ring is inert (`microbit_rootauth`), `Thread::privileged` being a **software field**.
- **Capability tracks hardware, and nothing marks the boards that cannot enforce**: no tier, no status
  field, no knob, exactly as `KICKOS_HAVE_MPU=1` on a chip with no `mpu.cmake` is a configure
  `FATAL_ERROR` rather than a silent no-op. What this record does instead is state per board what the
  hardware does, and the ring classification is per BOARD and enumerated, an unclassified armv6m board
  being a `FATAL_ERROR` rather than a vacuous pass.
- **Each gate was checked to FAIL**, a gate proving a guard exists being worth much less than one shown
  to fire. `shutdown_priv`: with the check removed the harness fails on a truncated TAP stream.
  `writable_global`: failed on exactly the two broken postures before the fix. `authority_cap`: covers
  the *non-privileged* arm, which would otherwise have shipped unexercised until the first board ran an
  unprivileged root, the same vacuity trap `kernel_ctor_placement` fell into.
- **What no host gate answers, and this must stay stated**: the bus PV classification, whether a tabled
  block is CLOCKED, and any real chip's mask column are silicon-only.
- **REJECTED: `STIR` as the ring prober's register.** `CCR.USERSETMPEND` can legitimately make it
  unprivileged-writable and this tree writes it from the kernel already, so a landed store would prove
  nothing about the ring; it must be `SHCSR`, `ICSR` or `VTOR`. Two binaries, because the terminal PPB
  arm ends the process while `ringpriv` survives to print its verdict, and its runner asserts an EXACT
  arm count so an arm cannot be deleted with the gate still green.
- **Measurement kept here because it is recorded nowhere else.** The ring arm was first established
  under emulation, on a synthetic no-MPU unprivileged-root build under QEMU: an unprivileged access to
  the System Control Space took `CFSR=0x8200`, BFSR byte `0x82` with the **MMFSR byte `0x00`** so
  provably not an MPU fault, and `BFAR=0xE000E280`, which is `NVIC_ICPR0`, inside the SCS. It escalates
  to HardFault because nothing in this tree ever sets `SHCSR.BUSFAULTENA`. That established the fault
  is takeable and what it looks like; it is **not** the silicon witness, which is `f302nucleo`
  (`reference/boards.md`).
- **Deleting the knob turned a barely-built gate into an always-built one**, which argued for the
  deletion on its own, and what it bought is narrower than credit-taking would suggest: three images had
  no `rootfault` arm at all, and the RISC-V PMP one was a gate CI had never run. **It removed CI work
  rather than adding it**, the two flipped arms being deleted once measurement showed `rootfault` was
  the only test they carried that the base arms lacked.
- **The absence of a banner suffix witnesses nothing about posture**, the argument for deleting
  `", root unprivileged"` being precisely that it carried no information. A deletion is the hardest
  change to witness, so name the discriminating witness instead: `rootfault` on `frdmk64f`, where SYSMPU
  `RGD0` stays supervisor-`rwx` so the isolation fault is reachable ONLY from a user-mode root. The same
  rule governs skip counts: **read the two named transcripts against each other, never the two
  totals**, a plan count moving for its own reasons.
- **Remaining gaps, each needing something no gate can supply.** `esp32-wroom` could not be given a run
  gate at all, upstream QEMU having no ESP32 machine, so the LX6 keeps build-only coverage for an arm
  that needs no hardware. Two more are structurally unfillable by emulation: v6-M MPU programming
  (QEMU models no Cortex-M0+ and no Cortex-M23 core) and the M7 speculation class.
