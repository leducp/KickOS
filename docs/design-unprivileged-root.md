<!-- SPDX-License-Identifier: CECILL-C -->
# Unprivileged root -- start unprivileged holding capabilities

> **Status: ACTIVE** -- **all five stages have landed and are merged** (stage 5 as `dde73ca`,
> PR #6). `KICKOS_ROOT_PRIVILEGED` no longer exists, so **every board boots an
> unprivileged root by construction**: there is no second posture to select, and no build in the
> tree can select one. Six boards are additionally witnessed on silicon, covering every enforcement
> backend -- `xmc4800-relax` (PMSAv7), `esp32c6-wroom` (RISC-V PMP), `pizero2350` (PMSAv8), `rx72m`
> (RXv3), `f411disco` (PMSAv7) and `frdmk64f` (SYSMPU), which is the first board to run its
> **full** service list unprivileged (console `k64uart` + SPI `k64dspi`) rather than a console-only
> one. The per-board captures are in `reference/boards.md`. The **ring** arm is witnessed too, on
> `f302nucleo` (2026-07-30, `ringpriv` `PASS (5 arms)`), by the `user/apps/common/ringpriv` prober
> section 10 describes. What remains is witness debt rather than design: `pizero2350` owes
> `rootfault` + `rootauth`, `f411spi` on `f411disco` is the last mux-write witness, and the
> `f302nucleo` fault-reporter root cause is open. See
> `design/README.md` for the marker taxonomy. The actionable
> checklist is `TODO.md`; this record is the reasoning behind it, which `TODO.md` does not carry.

Root is the kernel's first application thread: `kmain` creates it, it runs the app and
library constructors, then calls `kickos_init_entry`. It **was** privileged for its entire
life, holding every authority the kernel can grant for that whole time. It no longer is, on any
board and in any build: `KICKOS_ROOT_PRIVILEGED` was **deleted with no replacement**, so
unprivileged root is the only posture there is and there is nothing left to configure.
`ThreadAttr::privileged` defaults `false`, and `kmain`'s `cap_seat_authority(&g_root_tcb,
CAP_AUTH_ALL)` is unconditional. Section 4 carries that decision and what it cost.

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
7. Stage plan, and what each of the five stages landed
8. The four blockers
9. Limits, including the boards where this does not work
10. Verification: what witnesses what, and what nothing witnesses

**Where this landed, and what it still owes.** The design landed as **M4.5.6** and is merged,
squashed into `dde73ca`: the knob deletion and the write seam, the per-entry value mask and the
panic-message hardening, then the `esp32c6` `.data` LMA fix, thread-pool provisioning, and the ring
and sim seam gates. Captures below stamp the pre-squash tips they ran on; see the mapping in
`STATE.md`. **M4.5.7 -- removing the weak-symbol seam mechanism -- LANDED** in the same squash,
closing the sub-milestone: 33 `__attribute__((weak))` definitions became one-symbol
fallback TUs (`.weak NMI_Handler` a file-local label in 11 `startup.S` files, `arch_mpu_apply` a
plain non-overridable definition), 3 libc-interop symbols plus C++ COMDAT stayed weak behind
`tests/weak_allowlist.txt`, and a four-leg CI gate (`seam_defaults`) runs on every board. It is
owned by that change; the rule is stated canonically in `arch/CMakeLists.txt:11-71`, and nothing
here describes it further.

Still owed by this design:

- `f411spi` on `f411disco` is the only remaining mux-write witness.
- The `f302nucleo` fault-reporter root cause is open, blocked on a physical ST-Link replug.
- `frdmk64f` and `bluepill-c8` need thread-pool right-sizing before `KICKOS_POOL_ARENA_ASSERT` can
  go fleet-wide.

No longer owed, and **not to be repeated as open anywhere in this record**: `rxdrv` (rx72m),
`c6blink` (esp32c6-wroom), the RING arm (witnessed on `f302nucleo`, 2026-07-30, `ringpriv`
`PASS (5 arms)`), and `xmcssc` **as a service** -- `KICKOS_SERVICE_LIST_ROOT_MMIO` is now EMPTY
(`CMakeLists.txt`), so `xmc4800-relax` defaults to its full service list under enforcement, and the
wire shows `[xmcuart] driver up (polled TX)` then `[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced,
HW CS on SELO0)` at `270b6fa`.

---

## 1. The decision, and the design it superseded

The earlier plan was a separate drop-privilege syscall: root boots privileged, does its bring-up,
and then demotes itself before calling `main`. That needed a per-arch backend to change a
*running* thread's privilege, a `thread_regions_recompose` to rebuild the MPU region set at the
moment of demotion, and it put Xtensa last because Xtensa has no ring split to demote across.

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
| lx6 (Xtensa) | nothing to set: the LX6 has no ring split, so an unprivileged thread is the same thread here |
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

`thread_regions_recompose`, the separate drop-privilege syscall, its per-arch backends, and the
Xtensa-last sequencing are **deleted, not deferred**. They were machinery for managing a
transition this design does not have; carrying them as "later" would imply they are still wanted.

`drop_priv` survives only as a **contingent, much smaller** item. It is the one mechanism that
gives "privileged bring-up, then self-confinement for life", which is exactly what the blocked
bring-up bodies in section 8 wanted. It is in scope only if the privileged-write seam family
(section 9) proves insufficient for them, and it did not: with `arch_periph_reg_write` built,
section 8 has no bring-up body left that needs privilege.

### `KICKOS_ROOT_PRIVILEGED` went too, with no replacement

**The knob is deleted outright.** No bypass, no porting helper, no
tier. Where the CPU has a privilege ring KickOS uses it from the first frame; where it has none
the kernel's authority checks still hold and there is nothing to configure either way. A porter
bringing a board up may flip a thread's `privileged` attribute locally while working, and that
must never land in master. The reason is that this is a kernel and not an application: a knob is a
posture someone can ship by accident, and the posture this one permitted was "root holds every
authority for the life of the system". Fewer knobs, fewer misconfigurations.

The one place the name survives in any CODE or BUILD file is a deliberate `FATAL_ERROR` in
`cmake/KickOSConfig.cmake.in`, which refuses a consumer project that still passes it. (The docs
still discuss the name historically in many places, and correctly -- "anywhere in the tree" would
be false.) A silently ignored knob is worse than a deleted one: a downstream build that keeps
setting it would go on believing it had a posture to choose.

Two limits on that, both measured. The refusal reaches **out-of-tree consumers only**: an in-tree
configure still passing `-DKICKOS_ROOT_PRIVILEGED=ON` silently ignores it, with nothing louder than
CMake's generic unused-variable warning, so a CI script pinning the old posture would get an
unprivileged root with no notice. And clearing the guard needs more than dropping the `-D`: the
first configure leaves the name in the consumer's cache as `UNINITIALIZED`, which still satisfies
`DEFINED`, so the guard re-fires on every reconfigure until `cmake -U KICKOS_ROOT_PRIVILEGED`
deletes the entry. The message names that incantation for exactly this reason.

The scramble-test build option was deleted alongside it with **no tombstone**: an old configure
that still switches it on silently builds a plain `consoledemo`, and nothing in the configure
output points at the `conreclaim` app that replaced it. That is the same silence class the consumer
guard above exists to prevent.

**The safe default flipped with it.** `ThreadAttr::privileged` was `true`
(`kernel/include/kickos/thread.h`), so an attribute struct that forgot the field minted a
fully-authorized thread. It is now `false`, with privileged spelled out at the one site that
needs it, and a forgotten field can no longer silently mint privilege. That cost nothing at
the sites that exist, because the kernel builds exactly three `ThreadAttr`s: `idle` already spelled
`privileged = true` out (`kernel/init/kmain.cc`), root dropped its knob derivation and takes the
new default, and the spawn syscall assigns the field from its caller's request either way
(`kernel/syscall/syscall_thread.cc`). The banner's `", root unprivileged"` suffix went with the
knob for the same reason the knob did: under one posture it was a constant, and a constant carries
no information.

**The deletion was order-blocked behind stage 3.** The block was mechanical:
`frdmk64f` under `KICKOS_HAVE_MPU` defaults to the `kickos_services_frdmk64f` service list, and
while that list's bring-up wrote MMIO from root it sat in `KICKOS_SERVICE_LIST_ROOT_MMIO`, whose
configure-time `FATAL_ERROR` refused an unprivileged root (`CMakeLists.txt`). With the knob gone
that refusal had no posture left to be conditional on, so it would have fired unconditionally and
the board's enforcement build would have **stopped configuring** -- the `build-boards-mpu` CI job,
not a bench inconvenience. Stage 3 moved those writes into the drivers and the list came off the
refusal list, which is what unblocked the deletion.

**That refusal is now conditioned on `KICKOS_HAVE_MPU`, and the change of subject is the point.**
It used to ask "is root unprivileged?"; it asks "is enforcement on?". The reason is measured rather
than tidy-minded: **with the MPU off an unprivileged root does reach MMIO** -- a cross-domain write
completed on a non-MPU `qemu` image -- so an unenforcing build cannot go dark on a root-MMIO
bring-up, and refusing it would refuse a configuration that demonstrably works. The gate's real
subject was never the posture but **enforcement**, and only under the knob did the two coincide.

**`KICKOS_SERVICE_LIST_ROOT_MMIO` is now EMPTY** (`CMakeLists.txt`): no service list is refused at
enforcement any more. `kickos_services_xmc4800relax`, the combined XMC console+SPI list, was the
last entry and came off -- the `xmcssc` bring-up had already moved into the driver thread and takes
FDR/BRG/CCR through the seam (section 8), and the lift was taken once that bring-up was witnessed as
a SERVICE on silicon: `[xmcuart] driver up (polled TX)` then `[xmcssc] SPI service up (USIC0-CH1 SSC,
IRQ-paced, HW CS on SELO0)` at `270b6fa`. So `xmc4800-relax` defaults to its **full** service list
wherever `KICKOS_HAVE_MPU`, not a console-only one. The list and its `FATAL_ERROR` gate stay in place
for the next board whose bring-up writes MMIO from root, because that failure is silent and total (a
dark board, no diagnostic) and must be caught at configure rather than at the bench.

**The invariant that leaves is "exactly one privileged thread, and it is `idle`",** and it is now
written down as a contract -- `root-unprivileged-idle-alone-privileged` in
`reference/invariants.md` -- rather than living only in this record.
Idle has to stay privileged because the instruction it exists to execute is not portably available
unprivileged: RXv3 `WAIT` is privileged (RXv3 ISA UM section 1.4.3) and RISC-V only optionally
permits U-mode `WFI`. Section 9 carries that argument. Anything else privileged after boot is a
bug, and section 9 is also why none can appear -- spawning a privileged child needs a privileged
caller, and idle runs no app code. The invariant could not be stated at all while the knob existed,
because a build could always answer "two, and one of them is root".

## 5. The authority capability: shape, seat, and the arithmetic behind it

The gates that read `privileged` now call `cap_check_authority(caller, AUTH_*)` and nothing
else. The privileged-implies-everything arm lives *inside* that function, so the rule is stated
once rather than at every site -- which is the stronger property, and precisely what makes the
stage-1 conversion behaviour-neutral: there is no call site left that could encode it
differently. The authority is a capability, `CapType::CAP_AUTHORITY`.

**It is poolless.** There is no refcount and no object behind it -- the capability *is* its
authority bits, which is why `obj` is free to carry them rather than naming a pool entry. That
single fact drives two API consequences worth stating, because both are easy to get wrong:

- It resolves by reading its reserved slot, and **never** through `cap_resolve_e`, which would
  treat `obj` as a pool handle: at best `nullptr`, and with the authority word living there it
  would be interpreting the authority as an index.
- `obj_ref_inc` and `obj_ref_drop` need explicit no-op arms. Both `default:` cases assert, so a
  missing arm is a debug trap rather than a silent leak -- which is the behaviour you want, and
  the reason the arms are explicit rather than relying on the default.

**The authority word lives in `obj`, and the six-bit set is what that funds.** `CapEntry.rights`
is a `uint8_t` with three bits already spoken for (`CAP_WAIT`, `CAP_SIGNAL`, `CAP_TRANSFER`),
leaving five -- one short of the set section 5.1 arrives at. `CapEntry` is a frozen 8-byte ABI and
the rights *byte* cannot grow, but a `CAP_AUTHORITY` entry is poolless and leaves `obj` unused, so
the authority word moves there and the struct keeps its size. This retires the "merge
`AUTH_PINMUX` with the clock-rate bit" plan the old ceiling forced.

Two consequences follow, and both are improvements rather than costs:

- **The two families stop sharing a numbering.** They are separate enums now -- `CapRights` over
  bits 0..2, `CapAuthority` over bits 0..5 -- so nothing has to keep them disjoint, and the old
  reason for splitting at bit 3 (`CAP_TRANSFER` is read *type-agnostically* at the delegation site,
  so an authority reusing bit 0 would give one bit two meanings) simply stops applying. The price
  is that an object right is no longer a *distinguishable* wrong value in an authority mask: bits
  0..5 are all real authorities, so the "non-authority bits" spawn refusal now catches only bits
  above the six.
- **`rights` is 0 on an authority entry**, which is also what keeps it free of `CAP_TRANSFER`.

The width is now bounded by `kos_thread_params::authority`, a `uint8_t` in the struct's padding,
rather than by `CapEntry` -- `obj` has room for 32. A seventh and eighth authority cost nothing;
a ninth needs that params field widened.

The move is safe by inspection of every `obj` read rather than by assumption: `obj` is never a
sentinel, never range-checked, never `memcpy`'d, never exposed to userspace and never traced, and
each read is dominated by an explicit type test or a `switch (type)` arm -- with **one exception**.
The delegation copy in `thread_spawn` (`kernel/syscall/syscall_thread.cc`) copies `obj` *and*
`type` with no type test at all, filtered only by `CAP_TRANSFER` in `rights`. So that byte was the
only thing preventing an authority cap being duplicated bits-and-all into a child table, and with
the word in `obj` a delegable authority cap would be a full forgery at child index `ci+1` -- an
index the authority slot is reachable at whenever `cap_count >= 2`. That copy therefore takes an
explicit `CAP_AUTHORITY` refusal **first**, and it is by type rather than by rights precisely so
that it does not depend on a byte this change repurposes. The refusal is behaviour-neutral on
landing: `cap_seat_authority` masks to the authority bits, so such a cap never carried
`CAP_TRANSFER` and already earned `-KOS_EPERM`.

**It is seated at `KOS_CAP_AUTHORITY` (index 2), which was already reserved.** So it costs
**zero dynamic capability slots on every board**, including the four with only 9 handles. That
is the whole reason it is a rights-bearing cap at a reserved index rather than a new object.

**It is non-delegable twice over.** The delegation site refuses the `CAP_AUTHORITY` *type*
outright, and the cap carries no `CAP_TRANSFER` for the rights check to admit either. Index 2
therefore has exactly one writer, `cap_seat_authority`, reached only from `kmain` and from a
parent's `kos_thread_params::authority`. This closes the forgery question completely and defers
the delegation-packing collision in section 9 rather than walking into it.

**Per-instance capabilities were refuted on arithmetic, not on taste.** The alternative was a
capability per muxable pin (or per clock, per line) instead of one bit per authority *class*.
There are roughly 100 muxable pins on the larger parts against a 16-slot capability-table
ceiling. It does not fit, and it does not nearly fit, so the class granularity is forced.

### 5.1 The authority set, re-cut into six

**The old combined device authority bit is rejected.** It meant "console publish, shutdown,
reboot", which holds together only while root does all three. Once root is *only* a spawner,
publishing a console and ending the system belong to **different** threads, so no single bit can
carry both without handing the console driver the power to end the system.

The cut is six bits:

| Bit | Gates | Who holds it |
|---|---|---|
| `AUTH_MEMORY` | `ram_alloc`, the MMIO window grant, `mem_self_grant` | root, the spawner |
| `AUTH_PINMUX` | `pinmux_set` | root, for the board pin map; plus an app muxing its own pins |
| `AUTH_PSTATE` | `cpu_clock_set` | a CPU-governor service, and nothing else |
| `AUTH_IRQ` | `irq_attach`, `irq_unmask` | drivers with lines |
| `AUTH_SYSTEM` | `shutdown`, `reboot` | root / init |
| `AUTH_CONSOLE` | `console_publish` | root, during service bring-up |

The combined device bit and the separate clock bit both cease to exist as names. **`cpu_clock_set`
keeps a bit of its own** precisely because a governor service needs clock-rate authority and
nothing else: folding it into the lifecycle bit would hand the governor the power to end the
system.

**The console driver does not hold `AUTH_CONSOLE`, and that is not a future correction to make.**
`kos_console_publish` is called by **root**, inside the service list's `start()` body
(`xmcuart.cc`, `k64uart.cc`, `service_list_sim.cc`); the unprivileged child it then spawns only
*receives* on the endpoint. So the split earns its keep immediately rather than in anticipation:
root needs `AUTH_CONSOLE` for the length of bring-up and drops it before `main`, while keeping
`AUTH_SYSTEM` for the shutdown that ends a returning app. One bit could not express that.

**`AUTH_CONSOLE` has to be a bit, and specifically cannot be possession-gated** the way
`arch_periph_enable` is (section 7). `KOS_SYS_ENDPOINT_CREATE` is completely ungated, so any thread
can mint the endpoint it would publish; and `cap_console_publish` has no owner check and no
once-only guard, so a publish drops the kernel's existing ref and re-points `g_stdout_target`,
silently stealing a live console. `AUTH_CONSOLE` is the sole thing preventing that. The
missing owner check and once-only guard are wanted regardless of which bit gates the call, and are
filed separately in `TODO.md`.

### 5.2 Dropping an authority: `kos_cap_narrow`

`kos_cap_narrow(cap, mask)` ANDs an authority cap's word with `mask`. It can only clear bits, so
it needs no subset check the way a delegation mask does -- widening is not expressible. Narrowing
to 0 empties the slot rather than leaving a zero-word entry, because `cap_check_authority` reads
the word only after a type test, and an entry that still claimed the type would answer for a
capability nobody holds.

**It is ungated, and it has to be.** An authority required in order to *drop* authorities would be
one no thread could ever give up, which is the opposite of the property being built. It can only
clear bits in the caller's own table, so there is nothing for a gate to protect.

**It refuses any cap that is not the authority cap** (`-KOS_EINVAL`). Narrowing an object cap is
not merely unimplemented: dropping `CAP_WAIT` from an endpoint cap has to run the `recv_holders`
accounting that `obj_close_protocol` performs on close, or the last-receiver `EPIPE` wake goes
wrong. Nothing asks for it yet. The `cap` argument is kept in the signature so that generalisation
costs no ABI change.

**Where the narrowing happens.** The default `kickos_init_entry` narrows root *after* the pin map
and the service list -- so bring-up still holds `AUTH_PINMUX` and `AUTH_CONSOLE` when it needs
them -- and before `kickos_app_main`. The mask is whatever the app declared through
`kickos_app_authority()`, whose fallback (`system/init/app_authority_default.cc`, never a weak
symbol) is `AUTH_MEMORY | AUTH_SYSTEM`: spawn worker threads,
and end the system when `main` returns. An app needing more states it in its own translation unit
with `KICKOS_APP_AUTHORITY`, which is per-*executable* -- the alternative, a CMake variable, is one
value per build tree, and a single tree links `selftest` and `stress` against the same kernel.

An app whose `main` returns must keep `AUTH_SYSTEM`: `root_entry` ends the system with
`kos_shutdown`, and a refused shutdown reaches `KICKOS_UNREACHABLE("root: shutdown refused")`,
which panics with that text on the reclaimed UART. The bit is therefore *not* forced back on --
the failure is already legible, and forcing it would deny a never-returning app the ability to
declare 0 and hold nothing at all.

**The seat is unconditional, so the narrow always has something to narrow.** `kmain` seats root
`CAP_AUTH_ALL` at index 2 on every board and in every build, which is what makes this confine an
app **everywhere** rather than only where a knob was flipped. The empty-slot case the knob used to
produce -- narrow answers `-KOS_EBADF`, init tolerates it -- has no way to arise from the kernel's
own boot path any more, and the tolerance survives only for a thread that narrowed itself to 0 and
then narrowed again.

The privileged short-circuit inside `cap_check_authority` still exists -- it returns true on
`Thread::privileged` before reading the cap at all -- but the only thread that can take it is
`idle`, which runs no app code and holds no capabilities. So the bypass is unreachable from
anything that could want it, which is a stronger statement than the knob ever permitted.

## 6. The actual win: revocation

Confining root's *memory* is worth something, but it is not the main prize. The prize is that
**an authority can be dropped**.

Today root holds every authority for its entire life: it pin-muxes the board during bring-up
and then keeps `AUTH_PINMUX` forever, through the whole run of the application. With the
authority in a capability, root can drop `AUTH_PINMUX` after bring-up and before calling
`main`, and the kernel will then refuse a pinmux request from it -- including one made by
application code that has gone wrong.

That is `kos_cap_narrow` (section 5.2), and it is the app-facing point of the whole exercise.
Without it an unprivileged `main` can still ask the kernel to do every privileged thing there
is, which would make this work a memory-isolation change and nothing more.

## 7. Stage plan, and what each of the five stages landed

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

With that set witnessed the per-board phase was over, so the knob had no remaining job and stage 5
deleted it (section 4). Two consequences landed in the build rather than in the kernel:
`xmc4800-relax` stopped picking between two service lists by posture and now takes ONE list wherever
`KICKOS_HAVE_MPU` -- the full console+SPI list, once the root-MMIO refusal emptied (section 4) --
and the confinement gate stopped being opt-in (section 10).

`f411disco` came last because what stood in the way was a **pre-existing bench debt rather than
flip work**: PMSAv7 enforcement had never been witnessed on that board at all, so a flip there
would have had no enforcing baseline to be discriminating against.

**Stage 3 -- `arch_periph_enable(base)`. LANDED.** `int arch_periph_enable(uintptr_t base)`
(`arch/include/kickos/arch/arch.h`) ungates the block's clock and drops its bus-side
supervisor-protect, with a `-KOS_ENOSYS` fallback in `arch/common/arch_periph_enable_default.cc`
(`kernel/time/clock_select.cc` no longer defines any seam). Syscall
`KOS_SYS_PERIPH_ENABLE = 39`, wrapper `kos_periph_enable`. Backends: mk64f (UART0, DSPI0) and
stm32f411 (SPI1, the clock gate alone -- that bus exposes no privilege-classification register in
this tree). ESP32-C6 and RX72M deliberately have none: their windows need nothing from the seam, the
C6's APM open being a boot-time chip act in `arch_init` rather than a per-block request.

This is what `frdmk64f` was waiting for, and why it could not be a stage-2 target: its `k64uart` and
`k64dspi` PACR writers (`AIPS0_PACRN` at `0x4000_0064`, `AIPS0_PACRF` at `0x4000_0044`) both fall
inside `arch_reserved_blocks`'s AIPS0 entry (`[0x4000_0000, 0x4000_1000)`), so no grant can ever
reach them and the seam is the only way in.

**Each backend is a hand-curated per-chip table keyed on the EXACT block base, never a range**, and
both writes are *derived* from `base`, so a caller can never name a shared block's register or the
bit inside it. Which bases earn an entry is a hardware question, and the K64F PIT is where the
answer is no (section 9).

**The gate is possession, and there is no authority bit.** `caller_holds_mmio_block(base)`
(`kernel/syscall/syscall_mem.cc`) requires the caller to hold a live `ARCH_MPU_DEV` region whose
base matches exactly; a privileged caller bypasses it, exactly as inside `cap_check_authority`.
Deliberately **not** `user_range_ok`: that funnel's question is whether the kernel may dereference a
user pointer, and it passes trivially at `len == 0`. Exact base rather than containment is what
stops a sub-block window reaching a whole-block table entry.

**Gating it on an authority bit instead is the obvious-looking alternative, and it is wrong.** The
seam's callers are the bus drivers, so the lifecycle bit -- `AUTH_SYSTEM` under the cut in 5.1, and
the old combined device bit before it -- would hand `kos_shutdown` and reboot-to-bootloader to
every unprivileged driver in the fleet; every other bit carries collateral just as unwanted, and a
bit of its own would have to be justified against the same possession argument that removes the
need for one. Possession is *sufficient* because a granted window already
means "may enable, configure and disable this device" wherever the silicon permits it directly -- the
XMC `KSCFG` case in section 9 is the precedent -- so the seam brings K64F and F411 to the parity that
window granularity already grants there.

**The call site is the driver, not root, and that is what makes the seam's bound real** rather than
aspirational; the argument is in section 9 under *The privileged-write seam family*. On `frdmk64f`
root writes **no MMIO at all**.

Two supporting pieces landed with it. `arch/arm/chip/mk64f/regs/aips.h` gives the three formerly
open-coded slot -> (`PACR` register, `SP` bit) derivations one home, with `static_assert`s pinning
slots 106 (UART0), 44 (DSPI0) and 55 (PIT -- derivation coverage only, no entry). And `stm32f411`'s
pinmux gained an encoding field: `func` bit 8 `PINMUX_OUT_HIGH` presets an output high, and a
non-output mode carrying the bit is refused `-KOS_EINVAL`. `f411spi` needs it to hold the onboard
gyro's `PE3` chip-select deasserted so the gyro's SDO stays tri-stated. **`BSRR` is written before
`MODER`** (proven in the disassembly: the `str [r3,#24]` precedes the `str [r3,#0]`), because a
`BSRR` set on a still-input pin is inert while the reverse order asserts the `ODR` reset level first.
Known limitation: the encoding cannot reach `OSPEEDR` or `PUPDR`, so the old high-speed slew write
for `PA5`/`PA6`/`PA7` is gone and those pins run at the reset-default low-speed slew. `BR=/64` puts
SCK at ~1.3 MHz (84 MHz APB2 / 64, both from the tree); that the default slew carries that rate is
**engineering judgement, pending a DS9716 check** -- unlike the electrical facts cited above it rests
on no line in this tree.

**Stage 4 -- the app story. COMPLETE.** `kos_cap_narrow` (`KOS_SYS_CAP_NARROW = 40`), the
**six-bit authority re-cut** and the authority word's move into `CapEntry.obj` all landed together,
because they are one ABI change and not three: the cut decides the mask width, and the move is what
funds the cut. Section 5 carries the shape and 5.2 the narrowing. What the stage settled beyond the
plan:

- **The delegation copy took a type refusal first**, before the word moved. Without that ordering
  there is a window in which an authority cap is forgeable into a child table.
- **The narrow mask is declared per app, not per build tree.** `KICKOS_APP_AUTHORITY` in the app's
  own TU, defaulting to `AUTH_MEMORY | AUTH_SYSTEM`. A CMake variable cannot express it: one build
  tree links every app against one kernel.
- **`stress` is NOT declared privileged-root.** Its three privileged spawns were the same incidental
  leftover `selftest`'s two were, from the same original TAP-harness commit. Nothing in
  `ping`/`pong`/`churner` touches a privileged or authority-gated path, and `sleeper` -- unprivileged
  in that same app, in the same round -- already does a superset of what `churner` does. They are now
  unprivileged. Measured at the time against the then-existing knob: the old flags were refused
  `-KOS_EPERM` with root unprivileged, so `sim_stress` went from FAIL to PASS. Since stage 5 that is
  the only posture there is, and the fix is what keeps `sim_stress` buildable at all.
- **The in-env witness is the selftest's `authority_cap`**, whose worker drops its only authority and
  is then refused by the gate that had just answered for it. The *root* narrow has no in-env carrier
  for the same reason section 10 gives elsewhere, so it was witnessed by removing `AUTH_CONSOLE`
  from `initdemo`'s declaration: `console_publish` then failed from root on `qemu` with root
  unprivileged, and the identical source passed with root privileged. That was a **discriminating
  A/B taken while the knob existed and is not reproducible now** -- the standing gate that replaced
  it is `rootauth`, which discriminates within one posture by declaring some bits and withholding
  others (section 10).

**Stage 5 -- `KICKOS_ROOT_PRIVILEGED` deleted. LANDED** and merged (`dde73ca`). The knob went
with no replacement and no porting escape hatch; section 4 carries the decision and its
reasons. Three things travelled with the deletion:
`ThreadAttr::privileged` became `false` (`kernel/include/kickos/thread.h`), every `#if
KICKOS_ROOT_PRIVILEGED` site lost its condition rather than its body, and the invariant that
leaves -- **"exactly one privileged thread, and it is `idle`"** -- became statable, which is why it
is now the contract `root-unprivileged-idle-alone-privileged` in `reference/invariants.md` and no
longer only a paragraph here.

**Its one ordering constraint was discharged before the deletion, not by it.** With the knob gone
the root-MMIO service-list `FATAL_ERROR` had no posture left to be conditional on, so it would have
fired unconditionally and stopped `frdmk64f`'s enforcement build from configuring at all -- the
`build-boards-mpu` CI job. Stage 3 retired that board's root MMIO writes and its service list came
off the refusal list first. The refusal now keys on `KICKOS_HAVE_MPU`, which section 4 argues is its
real subject, and `xmc4800-relax` took one unconditional service list at enforcement instead of two
by posture -- the full console+SPI list, since the refusal list is now empty (section 4).

**The payoff is measured** (section 10). `user/apps/common/rootfault/CMakeLists.txt` used to
register the confinement gate under `KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND NOT
KICKOS_ROOT_PRIVILEGED`. With the third term gone the gate registers wherever `KICKOS_HAVE_MPU`,
which turns it into an **always-built gate on six runnable images**: `sim`, `qemu`, `qemu-m3`,
`qemu-m7`, `qemu-m33` and `qemu-riscv`. The `sim` arm is easy to miss and belongs in the count:
`KICKOS_HAVE_MPU` is 1 there by arch, and its `mprotect`'d arena faults on the cross-domain write
exactly as an MPU does -- what the sim cannot witness is the CPU-mode half (section 10), which
`rootfault` does not claim.

`selftest` used to declare itself privileged-root and **came off that declaration**. Its only two
privileged spawns were in
`rr_interleave`, and they turned out not to be a requirement at all -- the test wants two
threads interleaving under round-robin, which privilege has nothing to do with -- so they were
made unprivileged rather than pinning the whole suite to a posture. That matters beyond the one
app: selftest is the thing that *witnesses* the boundary, so leaving it privileged-root would have
made that boundary untestable on exactly the boards that adopt it.

**`mpu_privileged_guard` was deleted rather than carried forward**, together with the
`SKIP_TEST_IF_ROOT_UNPRIVILEGED` macro that existed to skip it. This is a reasoned retirement and
not a cleanup, so the argument is worth keeping. Once the knob was gone, the test's registration
condition and its skip condition were **both exactly `KICKOS_HAVE_MPU`** -- so it would have skipped
in 100% of the builds in which it existed, a case the suite cannot distinguish from a test that does
not exist. And it could not be repaired, because its premise is unreachable: it needs a privileged
thread running a test, and the only privileged thread left is `idle`, which runs no tests. What
replaces it is `rootfault`, which makes the **stronger** claim -- not "a privileged thread is
exempt from confinement" but "root is confined" -- and which now runs on all six images rather than
in no default build at all.
Any `n/60` selftest fraction elsewhere in this record is stale by this change: the suite plans
`1..66` and this test is not among the cases (measured, `frdmk64f` on its full service list and
`pizero2350`, both 2026-07-30).

## 8. The four blockers

**No service bring-up body pokes MMIO from root any more.** All three are retired:
`system/driver/mk64f/k64uart/k64uart.cc` (AIPS PACR) and
`system/driver/mk64f/k64dspi/k64dspi.cc` (clock gates, pin mux, GPIO, DSPI config) each call
`arch_periph_enable` from the driver thread that holds the window, which is why `frdmk64f` runs its
full service list with root writing no MMIO at all; and
`system/driver/xmc4800/xmcssc/xmcssc.cc` (USIC kernel clock, baud, protocol) now runs its whole
bring-up in the driver thread too, taking FDR/BRG/CCR through
`arch_periph_reg_write` (`KOS_SYS_PERIPH_REG_WRITE = 42`), the privileged-WRITE member of the
seam family section 9 describes. `xmc_spi0_start` is an endpoint-plus-spawn shim.

**From root** was the operative phrase for the K64F pair. On the XMC it was not enough: section 9
measures that three of the registers are refused to an unprivileged thread even inside the granted
window, so that bring-up needed both the move into the driver thread and a seam for those three.
The seam is possession-gated on the block base exactly as `arch_periph_enable` is, but its chip
backend is an ALLOWLIST of exact register addresses, so holding the window still does not make the
block writable. The `KICKOS_SERVICE_LIST_ROOT_MMIO` listing for `kickos_services_xmc4800relax`
has since been **retired** (section 4): `xmcssc` is witnessed as a service on silicon, the list is
EMPTY, and the full console+SPI list is the board's enforcement default. Nothing here is pending.

**`root_entry` read argv from the kernel stack.** Fixed in stage 0. Recorded here because of
*how* it would have failed: an unprivileged root faults on its first statement after the ctor
walk, on every enforcing board, and the sim reproduces none of it.

**`user_writable_ok` had no static-data arm.** Fixed in stage 0. Without it, root's writable set on
the five chips with no MPU backend, and on the sim, would have been "its own stack and nothing
else".

**The panic path's console reclaim was gated on a handover, and a fourth blocker hid behind that.**
`kpanic_enter` called `arch_console_reclaim` only while the console was `USER_OWNED`, but the
ownership axis records a PUBLISH and says nothing about whether the device is GARBLED. A thread
granted the console window can gate the channel clock (`KSCFG.BPMODEN=1, MODEN=0`) with no publish
at all, leaving the state `KERNEL_OWNED`, the reclaim skipped, and the panic banner written into a
dead channel -- the exact silent-panic-loss the reclaim exists to prevent. The gate is now
`state != RECLAIMED`, i.e. reclaim from any state, once.

**This promotes the "harmless-idempotent" reading of that branch from an aside to the
justification.** Widening the gate means the reclaim now runs on devices no driver ever touched, on
every posture and every board. The only reason that is safe is that every chip body is
IDEMPOTENT ABSOLUTE STORES, which `arch.h` requires of them; all three implementations were audited
against it. If a future body ever reads-modify-writes, or assumes a driver had configured the
channel, the widened gate breaks it -- so that requirement is now load-bearing and must be stated
whenever a new body is written. Storing `RECLAIMED` BEFORE the call is the second half: it makes the
reclaim unconditional-ONCE, which both stops a fault inside the body from recursing (a pre-existing
hazard) and stops a body from truncating the banner it just printed. Witnessed by `conreclaim` on
`xmc4800-relax` at `c5d9b0d`; the reclaim BODIES remain silicon-only on both chips that have one.

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

  Consequences, and what was built. `xmc_spi0_start`'s FDR/BRG/CCR stores need a privileged
  executor; the KSCFG/SCTR/TCSR/PCR/PSCR/DX0CR/INPR rest of the sequence is grant-reachable and
  needs nothing. That executor is `arch_periph_reg_write(base, offset, value)`
  (`KOS_SYS_PERIPH_REG_WRITE = 42`), and given it the `xmcssc` bring-up **moves wholesale** into
  the granted unprivileged driver, and has to, because **root cannot hold a DEV region
  at all.** `ARCH_MPU_DEV` is attached to a live region in exactly one place, `domain_for`
  (`kernel/domain/domain.cc`), whose only MMIO-carrying caller is `thread_spawn` --
  `thread_create`'s in-kernel callers pass `mmio_base == nullptr` -- and
  `KOS_SYS_MEM_SELF_GRANT` hardcodes `ARCH_MPU_R | ARCH_MPU_W`. So root cannot be the seam's
  caller: the driver is the only thread that can hold the window, and a split sequence would leave
  a privileged half configuring a channel its own caller cannot address. Stage 3's
  `arch_periph_enable` does not cover it: that seam is "ungate a clock, drop supervisor-protect",
  the K64F/C6 shape, whereas this is USIC-specific FDR/BRG/CCR programming.
  The failure mode with no seam is the bad kind -- an unprivileged root would program the baud
  generator, take no fault, and leave the channel clocking at whatever the previous value
  implies. Which is also why every seam call site in the tree **reads the register back**: the
  discard is silent in both directions, so the read-back is the only evidence a store landed.

  **The gate is possession, and the backend is an allowlist of exact register addresses.** Those
  two together are the whole design. Possession is the same `caller_holds_mmio_block(base)` stage 3
  uses, for the same reason (section 7): where the silicon puts a block's controls inside the
  window, holding the window already carries the authority. But possession must NOT become blanket
  write access to the block, because that is precisely what the bus classification withholds -- so
  the chip table keys on `(base, offset)`, and a base or an offset it does not name is
  `-KOS_EINVAL`. A chip with no backend answers `-KOS_ENOSYS` from a one-symbol archive member
  (`arch/common/arch_periph_reg_write_default.cc`), NOT a weak symbol; `reference/porting.md`
  carries the link-order rule that makes a mis-resolution a link error instead of a silent
  decline.

  **The seam is witnessed, and by the same probe that found the refusal** -- XMC4800 Relax,
  `KICKOS_HAVE_MPU=1`, 2026-07-30, `MinSizeRel`, banner `mpu enforce`. That single run is what makes
  it evidence rather than an assertion, because it puts the seam and the bare store **side by side
  in one unprivileged thread holding one window**, and it does it twice over with two bit patterns
  so that neither outcome can be the register's resting value:

  - The seam writes pattern B and reads it back: `seam FDR: rc=0 wrote=0x2aa read=0x2aa exact`,
    `seam BRG: ... 0x2aa0000 ... exact`, `seam CCR: ... 0x2000 ... exact`.
  - The same thread then stores pattern A **directly** to the same three registers and all three
    are refused silently: `unpriv FDR[PV]: pre=0x2aa writing 0x155 ... post=0x2aa DROPPED (post ==
    pre)`, and likewise `BRG` and `CCR` (`pre=0x2000 writing 0xc001 ... post=0x2000 DROPPED`).
  - The seam is then handed **the very value the direct store failed to land**, and lands it:
    `seam CCR: rc=0 wrote=0xc001 read=0xc001 exact`, with `FDR` `0x155` and `BRG` `0x1550000` the
    same.

  Both controls ride in that run as well. Positive: `unpriv SCTR[U,PV control]: pre=0x3030100
  writing 0x7070101 ... post=0x7070101 LANDED`, so the window is genuinely held and writable.
  Negative: the run ends on an ungranted SCU poke that MemManages, `CFSR=0x82`,
  `MMFAR=0x50004648`, so enforcement is genuinely on. And the **refusals are measured on hardware**
  rather than reasoned about: `refusals: off-allowlist rc=-22 (want -22), unheld-window rc=-1 (want
  -1)` -- `-KOS_EINVAL` for a register the chip table does not name, `-KOS_EPERM` for a caller that
  does not hold the window. So the allowlist bound and the possession gate are both witnessed at the
  same time as the seam they bound.

  **The register sequence is witnessed end to end, separately from the probe.** `xmcspi` takes the
  identical three writes through the seam and then runs a real SSC loopback: `seam FDR: rc=0
  wrote=0x816f read=0x39b816f LANDED`, `seam BRG: ... 0xd3c00 ... LANDED`, `seam CCR: ... 0xc001
  ... LANDED`, then four words echoed and `loopback PASS (all words echoed equal)`. That closes the
  gap a read-back alone leaves: a store can land and still be the wrong value. It also validated the
  one literal the implementation could not justify from the manual -- `FDR`'s `RESULT[25:16]` field
  drifts under a running baud generator, so a naive read-back comparison would report a false
  DISCARDED, and the `FDR_RESULT_MASK` that excludes it returned `LANDED` correctly on silicon.
  What `xmcspi` does **not** witness is `xmcssc` **as a service**. That has since been witnessed
  separately, on the board's full service list: `[xmcuart] driver up (polled TX)` then `[xmcssc] SPI
  service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)` at `270b6fa`, which is what retired the
  root-MMIO refusal (section 4).

  This record previously read the measurement as "the bring-up cannot move wholesale". It is the
  opposite, and the correction is worth keeping because it is what settles who calls the seam:
  with no path by which root holds a DEV region, there is exactly one candidate.

  The earlier "contradicted by this repo's own silicon" reading was **invalid inference, not a
  measurement**. It rested on `consoledemo`'s scrambler writing FDR/BRG/CCR from
  `privileged=false` and the UART coming out garbled. But the scrambler also writes `SCTR`,
  `TCSR` and `PCR` (all `U,PV`) and gates the channel clock via `KSCFG`, and any one of those
  alone garbles the UART -- so the garbling never required FDR/BRG/CCR to land. It is now known
  they did not. That scrambler no longer lives in `consoledemo`: it was restaged as its own app,
  `conreclaim`, and the scramble-test build option it hung off does not exist any more, so the
  original artifact is not there to re-read.

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
  this device". This is **orthogonal to root's privilege** -- it is equally true of any unprivileged
  window holder, and was true before root was one -- and a property of window granularity, not a
  regression. A window that happens to span two peripherals hands over both.

  **This is the precedent stage 3's possession gate rests on** (section 7). Where the silicon puts a
  block's enable *inside* the window, holding the window already carries the authority to enable the
  device, so a seam reaching only the caller's own block adds nothing the grant did not already
  imply: it brings K64F and F411 to the parity the XMC gets for free from its register layout.

  The grant is an **upper bound**, not the whole window: where a bus enforces its own per-register
  privilege classification, the holder gets strictly less, and on this chip it learns so
  silently (the FDR/BRG/CCR measurement above). So the two limits compose -- window granularity
  sets what is reachable, the bus decides what is writable -- and neither is visible from the
  grant call.
- **An `arch_periph_enable` entry exists only where the bus gate's granularity is contained by the
  block the window covers, and the K64F PIT is where that fails.** One AIPS `PACR` slot governs a
  whole 4 KiB block, so opening slot 55 for the legitimately granted PIT channel-2 window
  (`0x4003_7120`) would equally expose the chained ch0+ch1 pair that carries `arch_clock_now` -- the
  registers `arch_reserved_blocks` protects by address (`{PIT_BASE, 0x120}`). So there is no PIT
  entry, and that base answers `-KOS_EINVAL`. **The refusal is the design working, not an
  omission**, which is what separates its consequence from `f411spi`'s: `k64drv` cannot run
  unprivileged *by design*, where `f411spi` was merely waiting for a seam. And since M4.5.6 left no
  privileged root anywhere, that is no longer one posture's cost -- `k64drv` has no path left at all
  and cannot run, which is a decision to take about the app rather than a gap to close
  (`m2-readiness.md`). `arch_periph_reg_write` does not rescue it: the obstacle is the AIPS gate's
  4 KiB granularity, not the privilege of the store.

  That a K64F slot's effect is system-wide once opened is the **already-recorded hardware ceiling**,
  not a finding of this stage: `book/peripheral-isolation-and-the-hardware-ceiling.md`,
  `reference/architecture.md` (*Peripheral (MMIO) isolation is hardware-bounded*) and the M3-era
  instance in `design-m3-console-handover-stageii.md` (*Isolation reality by target*), which is about
  this same PACR slot.

  The slot arithmetic has one home, `arch/arm/chip/mk64f/regs/aips.h`, with `static_assert`s pinning
  slots 106 (UART0), 44 (DSPI0) and 55 (PIT, derivation coverage only). Writing it down caught that
  the `PACR` offsets are **not** contiguous -- groups 0..3 at `0x20`..`0x2C`, `0x30`..`0x3C`
  reserved, groups 4..15 at `0x40`..`0x6C` -- so the obvious `0x20 + group * 4` names the wrong
  register for slot 44.
- **The CPU/peripheral clock coupling is over-generalised, and it should be a question asked of the
  chip.** `cpu_clock_set` refuses outright while a userspace driver owns the console
  (`kernel/time/clock_select.cc`), on the grounds that the kernel cannot re-derive a baud it no
  longer owns. That veto generalises from a biased sample: exactly **two** chips implement
  `arch_periph_clock_hz`, and both are coupled (`chip_xmc4800.cc`, fPERIPH = fCPU/2;
  `chip_mk64f.cc`, `SystemCoreClock` or /BUS_DIV). A chip with an independent peripheral root has no
  backend at all, so the decoupled case has never had to be stated -- and the assumption is baked
  into the seam's own contract wording ("retune the core/bus clock"). On a chip with a dedicated CPU
  PLL there is nothing to refuse. The right shape is a **notification to the affected services**
  rather than a veto, and the console is not the only affected service: drivers size their divisors
  off `kos_periph_clock_hz` too. M4.6 work, and a CPU governor depends on it.
- **`arch_reserved_blocks` reasons about addresses, and interrupt routing is not an address.**
  A granted USIC channel can re-point its `INPR` at a service-request node the kernel owns. No
  address-based admissibility check can see that, because every store involved is inside the
  window the grant legitimately handed over. Measured on silicon by
  `user/apps/xmc4800-relax/inprstorm`: an unprivileged holder of the U0C1 window reroutes
  `INPR`'s RINP/AINP onto SR0, the kernel console's node, and storms the channel without ever
  clearing `RIF`/`AIF` -- `TBUF0` from software in two profiles, and the TX FIFO draining
  autonomously in a third. The honest severity is a **bounded parasitic CPU tax, not a denial of
  service** -- the console kept beating and the board did not wedge.

  **The bound is the SCHEDULING model, not the baud**, and getting that right matters because the
  seam itself hands the attacker the rate knob: `FDR` and `BRG` are two of the three allowlisted
  registers, so a verdict resting on any single operating point would be a verdict the attacker can
  reconfigure. The earlier record did rest on one -- an instrumented `~37,700 console_tx_isr
  invocations/second` taken at the SSC 115.2 kHz profile. That figure is retained as what it is (an
  earlier instrumented measurement at one profile) and is no longer what carries the claim.

  What carries it is a THREE-point sweep across the knob's range, all at `270b6fa-dirty`, ending in
  the hardware-autonomous case that used to be the residual. The storm thread runs at priority 1,
  below root's 2. In the two software-paced profiles its loop writes `TBUF0` **one word at a time
  from software**, so its rate is capped by its CPU share however fast the channel shifts, and
  raising the shift clock ~625x changes nothing because the loop was never shift-clock-bound. Those
  two deliberately keep storming `TBUF0` as the comparison points; the third arms the FIFO instead.

  | Profile | `STEP` | `PDIV+1` | `PCTQ+1` | `DCTQ+1` | Distinct heartbeats | Steady `dt` | Last |
  |---|---|---|---|---|---|---|---|
  | 115.2 kHz comparison (`TBUF0`) | 367 | 14 | 1 | 16 | 143 | 300 ms | `t=42608ms` |
  | MAX RATE (`TBUF0`) | 1023 | 1 | 1 | 1 | 143 | 300 ms | `t=42608ms` |
  | FIFO autonomous drain + MAX RATE | 1023 | 1 | 1 | 1 | 143 | 300 ms | `t=42608ms` |

  Identical on every column, at `fPERIPH=72000000 Hz`: beats numbered 0 through 142 in each, beat 0
  at `dt=0ms` and every later beat at `dt=300ms`, last line `[inprstorm] heartbeat 142 t=42608ms
  dt=300ms`. **143 is the DISTINCT count**; each capture file holds 150 heartbeat lines because the
  serial reader duplicated seven of them ahead of the banner, so the line count is an artifact of
  capture and not of the run. That every `dt` is 300 ms is a DERIVED reading of the three captures --
  no capture reports an outlier verdict of its own. Captures: `c3-inprstorm.log`,
  `c3-inprstormmax.log`, `c3-inprstormfifo.log` in `.session/m456-silicon/`; the wire values are
  tabulated in `reference/boards.md` rather than duplicated here. The secondary reason the tax cannot
  compound is that the USIC receive service request is **edge** (one pulse per received word), so a
  held, uncleared flag does not re-assert the node.

  **The FIFO vector needed NO allowlist widening, which is what makes the third profile a stronger
  result than a wider grant would have given.** XMC4700/XMC4800 RM V1.3 Table 18-20 marks only
  `FDR`, `BRG` and `CCR` as Write=PV; `TBCTR` (108H), the `INx` push aperture (180H + x*4), `TRBSR`
  (114H) and `CCFG` (004H) are all `U,PV`. So the whole FIFO arming and refill path is what the
  CURRENT grant already permits, and the measurement is of the shipped grant rather than of a
  hypothetical one.

  **The RM subtlety is why the vector looked untested rather than negative.** Writing `TBUF0` (080H)
  addresses the STANDARD buffer and silently BYPASSES the FIFO; the push aperture is `INx` (RM
  p.18-223), and the FIFO auto-loads TBUF whenever `TCSR.TDV=0` (RM 18.2.8.4). The original
  `inprstorm` fed `TBUF0`, so it was never exercising the FIFO at all -- the earlier "no capture
  exists" reading was correct about the absence and wrong to treat the vector as merely unvisited.

  **Autonomy is proven BEFORE the null result**, so the third profile's flat cadence cannot be read
  as the FIFO never having run (`c3-inprstormfifo.log`, `270b6fa-dirty`):

  ```
  [inprstorm] CCFG (TB=bit7)=0x80cf
  [inprstorm] TBCTR=0x6000000 SIZE=64 LANDED (FIFO armed, no seam)
  [inprstorm] slow-divider preload TBFLVL=64, after 10ms no-fill=0 (backlog drains autonomously if falling)
  ```

  and its profile line is `STEP=1023 PDIV+1=1 PCTQ+1=1 DCTQ+1=1 fPERIPH=72000000 Hz`.

  **The reason, which is what the bound now rests on.** A backlog only builds while drain < fill, so
  the sustained SR0 rate is `min(fill, drain)` -- and `fill` is the attacker's sub-root CPU share
  refilling a FINITE 64-deep buffer. **FIFO depth and clock rate trade off; they do not multiply.**
  Depth buys a one-time burst of at most 64 words, after which the sustained rate falls back to
  whatever the storm thread can push. So the bound is no longer "established for software-paced
  storms only": it is STRUCTURAL, and hardware autonomy inside a finite buffer does not escape it.
  The verdict stands unchanged -- a bounded parasitic CPU tax, not a denial of service -- and the
  residual this bullet used to carry is CLOSED on silicon.
- **One holder per MMIO window, enforced at grant admission -- and the respawn edge that follows
  from it.** The domain *dedup* scan is skipped when a spawn carries MMIO
  (`kernel/domain/domain.cc`), so an MMIO grant always takes a fresh domain slot; that is a
  statement about domain identity, not about exclusivity. Exclusivity is its own test:
  `dev_window_free` walks every live domain's DEV regions and a request overlapping one already held
  is refused `KOS_EBUSY`. Matched on RANGES, not slots, via `grant_ranges_overlap` shared with Rule 7
  -- so an equal, containing or straddling window all refuse while an **adjacent** one stays
  admissible, which the `frdmk64f` PIT CH2 grant depends on (the chip's reserved PIT entry stops at
  `0x4003_7120` precisely so that grant can sit flush against it). Deliberately **not** a per-module
  count table: that would be hand-maintained and would drift. Sharp edge: a driver respawn issued
  before the dying driver's domain reference has dropped still sees the window live and gets
  `-KOS_EBUSY`, so a respawner needs join-before-respawn or a documented retry.
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
  the sole possible silicon witness for the ring arm -- and it has now TAKEN that witness:
  `ringpriv` `PASS (5 arms)`, 2026-07-30 (section 10). What is still open on the board is its
  fault-reporter root cause, blocked on a physical ST-Link replug.

  **`bluepill-c8` costs no coverage.** It is not materially different from `f302nucleo` -- both are
  64 KiB-flash armv7m parts with no MPU and a real privilege ring -- so the Nucleo carries the
  hardware coverage for that whole class, and a second unit would witness the same arm twice.

  The **9-handle provisioning costs this nothing**: the authority cap is seated at reserved
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
  The two boards are otherwise not one case: `bluepill-c8` carries its own `board_config.h`
  (`boards/bluepill-c8/`), while `f302nucleo` has no board directory at all and takes the chip's.
  Both take their chip's linker script.
- **No privileged thread can come into existence after boot.** Spawning a privileged child requires
  the caller be privileged, and that is deliberately not a capability. Root is unprivileged and
  `idle` runs no app code, so the privileged population can only ever shrink, for the lifetime of
  the system. With the knob deleted (section 4) this holds on **every** board and in every build,
  which is what makes it statable as an invariant at all
  (`root-unprivileged-idle-alone-privileged`, `reference/invariants.md`).
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
grow by argument. The reasoning is here; the contract both members must keep is
`privileged-write-seam-possession-and-allowlist` in `reference/invariants.md`. Two properties come with that, and they are what the shape buys. The seam's
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
   `FDR`/`BRG`/`CCR` case is the only confirmed member, and `arch_periph_reg_write`
   (`KOS_SYS_PERIPH_REG_WRITE = 42`) is its seam.

**The two seams differ in what the ABI carries, and that follows from (1) versus (2).** For class
1 the caller names only the block, and the register and bit are DERIVED from it, because the
register lives outside the window and naming it would be naming something the caller has no claim
to. For class 2 the caller names `(base, offset)`, because the register is inside its own window
and it already reads that address freely; withholding the name would buy nothing while making the
seam a per-register function on every chip. What replaces the derivation as the bound is the
**per-chip allowlist of exact register addresses** -- so the reachable command space is not "the
block I hold" but "the specific registers this chip's porter enumerated inside the block I hold",
which is strictly narrower than what a class-1 entry already grants.

**That justification names a premise the code did not originally establish, and the gap was a real
confused deputy.** "The register is inside its own window and it already reads that address freely"
is only true if something CHECKS it. `caller_holds_mmio_block(base)` compared the base and IGNORED
the region's size, so the only bound on the target address was the chip allowlist -- which bounds
which registers, not which of them this caller may reach. MEASURED from an out-of-tree probe,
identical on `qemu`+MPU, `qemu` no-MPU and `qemu-riscv`+MPU: a holder of a **32-byte** DEV window at
`0x40000000` passed the gate at `off=0x40`, `off=0x1000`, `off=0x0FFFFFFF`, and at an offset that
wrapped 2^32. On the flagship it was exploitable rather than theoretical: `arch_mpu_region_encodable`
admits a 32-byte DEV window at `0x40030200`, covering `CCFG`/`KSCFG`/`FDR`/`BRG`/`INPR`/`DX0CR` but
NOT `CCR` at `+0x040` -- so a holder that could neither READ nor WRITE `CCR` could have the kernel
write it privileged.

The fix restores the premise instead of abandoning the ABI. `caller_holds_mmio_reg(base, offset)`
requires the region matched by the exact base to also CONTAIN `[base + offset, +4)`; misaligned and
wrapping requests are refused `-KOS_EINVAL` BEFORE possession is consulted, since they are malformed
rather than unauthorised, and out-of-region is `-KOS_EPERM`. `arch_periph_enable`'s helper and
semantics are untouched -- it takes a block base and has no offset to bound. The in-env arm that
pins it is arm 3 of `periph_reg_write_unheld`, which the earlier shape could not express: `PRW_OFFSET`
was `0x2`, deliberately unaligned and unnameable, so it never tried an offset a table COULD name from
a window too small to contain it. It is now `0x4`, and the unnameability guarantee moved from the
offset to the BASE. Silicon confirms the containment check did not break the legitimate consumer
path: `pvprobe` re-taken at `c5d9b0d` still lands all three seam writes `exact` (section 9).

**Membership requires a measured hardware refusal, never a plausible reading of a manual.**
`user/apps/xmc4800-relax/pvprobe` is the artifact template, and what makes it evidence rather
than a symptom is that it carries **both controls in the same run**: a positive one (`SCTR`, in
the same window from the same thread, lands exactly) and a negative one (an ungranted SCU poke
MemManages). Without the positive control a dropped write is indistinguishable from a broken
grant; without the negative, from unenforced memory. A register with no such measurement behind
it is not in the family, however confident the reading -- which is why the K64F and C6 exemptions
above are recorded as unsettled rather than as findings.

**The same standard applies to the seam, and class 2 now meets it.** A membership measurement says
the silicon refuses the store; it says nothing about whether the seam that answers the refusal
actually writes. So `pvprobe` was extended to carry the seam and the bare store in the **same
unprivileged thread, the same held window and the same run**, and to hand the seam the very value
the direct store had just failed to land. Section 9 records the wire. `arch_periph_reg_write` is
therefore witnessed as **non-trivial** rather than assumed, and its two refusals -- off-allowlist
and unheld-window -- are measured on hardware rather than only in the in-env
`periph_reg_write_unheld` gate. Class 1 has no equivalent: `arch_periph_enable`'s positive arm still
has no probe of its own and is witnessed only indirectly, by the driver bring-up it makes possible
(section 10).

Stated plainly, because pretending otherwise would be worse: this is an **ioctl-shaped** family,
and it is chip-dependent. What bounds it is not elegance but reach -- the seam's reachable command
space is exactly the peripherals its caller has been granted, so a holder of one window can only
ever ask the kernel to configure that window's device.

**That bound holds because the call site is the driver rather than root.** It is a fact about the
code, not an aspiration: root holds no DEV region on any board -- `ARCH_MPU_DEV` is attached to a
live region in `domain_for` alone, whose only MMIO-carrying caller is `thread_spawn`, and
`KOS_SYS_MEM_SELF_GRANT` hardcodes `ARCH_MPU_R | ARCH_MPU_W` -- so root could not be the seam's
caller even if the design wanted it to be, and the thread holding the window is the only candidate.
A seam gated on an authority *bit* instead would carry no such bound at all: any holder of the bit
could name any base in the chip's table.

### The reboot capability

`kos_reboot` is built, as `KOS_SYS_REBOOT` behind the `arch_reboot` seam. It is specifically
**reboot into the chip's bootloader** -- flashing mode (RP2040 `_reset_to_usb_boot`; RP2350
bootrom `reboot` with `REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL`; imxrt1062 the `bkpt` the Teensy's MKL02
catches) -- which is why it sits behind `KICKOS_ENABLE_SELFTEST`: it is a developer affordance for
reflashing without touching the board, not a general system reset. A general reset is not designed
anywhere today; the mode decision below settles that it should be, and how. A chip with no
bootloader entry declines through the seam's `-KOS_ENOSYS` fallback
(`arch/common/arch_reboot_default.cc`, selftest-gated so it is absent from a production image)
rather than pretending.

**Decision: it shares `arch_shutdown`'s authority**, rather than taking a dedicated reboot
capability type at index 3 or a rights bit of its own. Two reasons: reboot-to-bootloader is the
same class of act as shutdown (end this system, hand the chip to something else); and index 3 is
the last free well-known index, worth more than bit granularity here. That shared bit is
`AUTH_SYSTEM` (section 5.1).

The counter-argument, recorded because it is real: shutdown merely stops execution, whereas
reboot-to-bootloader leaves the board accepting new firmware over USB. Fusing them means anything
permitted to end the system can also put the board into flashing mode -- acceptable *for a feature
compiled out of production images*. The sharper half of the objection, that a **console publisher**
inherits flashing mode, is answered by the re-cut rather than by a merge: `AUTH_CONSOLE` and
`AUTH_SYSTEM` are different bits.

**Decided, not yet applied: `arch_reboot` takes a MODE, and the compile knob gates the mode rather
than the seam.** Post-4.5.4 work, owned by M4.6. Four parts:

1. **The seam gains a mode argument**, at least a normal system reset and reboot-into-bootloader.
   `int arch_reboot(void)` (`arch/include/kickos/arch/arch.h:44`) takes no argument and means
   bootloader entry specifically, which is exactly what leaves a general reset with nowhere to live.
2. **The `-KOS_ENOSYS` decline becomes per-MODE, not per-function.** A chip that can reset but
   has no documented bootloader entry declines both today. The asymmetry that motivates the split is
   **ARM-specific, and architectural rather than measured**: `SCB->AIRCR` `SYSRESETREQ` is available
   on armv6m/v7m/v8m with no chip-specific code, so a normal reset could sit in `arch/arm/common`,
   whereas bootloader entry is per-chip and only three backends have one (rp2040, rp2350,
   imxrt1062). RISC-V has no architectural reset, and RX and Xtensa each have their own mechanism.
3. **The knob gates only the bootloader mode, and is renamed to an enable-reboot-to-bootloader
   spelling.** `KICKOS_ENABLE_SELFTEST` conflates "test-only syscall surface" with "may this image
   put the board into firmware-accept mode", and the conflation costs a real capability for no
   security gain: no production in-kernel path can reset the chip at all, while a privileged
   thread may need to sequence one -- watchdog recovery, a fault-handler reset, a bring-up
   retry -- none of which carries the bootloader's risk. **Not `..._IN_FLASH_MODE`**: the
   existing sibling knob is `KICKOS_SHUTDOWN_TO_BOOTLOADER` (`CMakeLists.txt:117`), and two knobs
   naming one destination differently is drift.
4. **Bootloader mode wants the knob AND an authority bit.** A normal reset is lifecycle, the same
   class as shutdown; bootloader mode leaves the board accepting firmware over USB, so
   belt-and-braces is proportionate for that one act and not for the other. With the modes split,
   `AUTH_SYSTEM` (section 5) covers normal reset without granting firmware-accept, and the knob is
   what the bootloader mode needs on top -- which is what the counter-argument above asks for.

Two pieces of existing debt retire with it. `KICKOS_SHUTDOWN_TO_BOOTLOADER` stops being a parallel
mechanism reaching `arch_reboot` through `bootloader_handover` (`kernel/init/console.cc:293`) and
becomes a POLICY on one seam: on shutdown, use mode BOOTLOADER. And the production ABI improves --
syscall 38 becomes a real production syscall taking a mode, answering `-KOS_ENOSYS` for a mode the
chip lacks and `-KOS_EPERM` without authority, instead of `-KOS_EINVAL` from the dispatch default
arm. That retires both the configure-time `FATAL_ERROR` (`CMakeLists.txt:124`) and the `abi.h:62-64`
annotation that exists only to document the compiled-out arm.

The symptom that made the conflation visible:
`arch/arm/chip/imxrt1062/chip_imxrt1062.cc:49` carries a local forward declaration of `kpanic`
inside a `KICKOS_ENABLE_SELFTEST` block, only because that chip's `arch_reboot` is a `bkpt` that
must not resume. A fundamental function's declaration sitting behind a test flag is the conflation
in one line.

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
  gates, which would otherwise have shipped unexercised until the first board ran an unprivileged
  root -- the same vacuity trap `kernel_ctor_placement` fell into.

**Stage 3's possession gate has a negative arm in the suite and a positive arm only on hardware --
and that is true of `arch_periph_enable` specifically, not of the seam family.**
`selftest`'s `periph_enable_unheld` asserts the refusal of a caller holding no window, and that runs
everywhere. `arch_periph_enable`'s positive arm has **no in-env carrier**, because no sim backend
defines it: the host resolves the declining default
(`arch/common/arch_periph_enable_default.cc`), so there is nothing on the far side of the gate to
reach. Driver bring-up and the two-arm probes on silicon are what witness it instead.

**`arch_periph_reg_write` is different, and the older "no in-env gate for the seam" reading is
retracted.** The sim implements it over a real host mapping, and the grant that carries the window
exists there -- `arch_mpu_region_encodable` is fail-closed on the sim for every address and every
shape EXCEPT the one published fake-register base at exactly `0x10000`. So `periph_reg_write_mask`
gates the allowlist match, the per-entry value mask, refuse-not-trim, and the kernel's containment
refusal (a tabled offset one word past the held window answers `-KOS_EPERM`, not `-KOS_EINVAL`, which
is what proves the refusal came from the kernel and not the chip layer) on the HOST, and
`periph_reg_write_unheld`'s arm 3 gates alignment and wrap there too. What the host still cannot do,
and this must stay stated: model the bus PV classification (that an unprivileged store is silently
DISCARDED remains a `pvprobe`-only fact), prove that a tabled block is CLOCKED, or check any real
chip's actual mask column. On `frdmk64f`, an unprivileged root with the **full** service list (`k64uart` +
`k64dspi`) runs `selftest` `1..66` with `# skipped: 1` and `# all tests passed (1 skipped)`, the one
skip being the pre-existing `ok 18 - mutex_deadlock # SKIP pool too small`, a genuine
`KICKOS_MAX_THREADS` constraint recorded in `reference/boards.md`. And `rootfault` denies root's
cross-domain write with `SYSMPU ISOLATION FAULT: port=3 addr=0x20015140 master=0 W EDR=0x80000003`
reported through an **imprecise** bus fault (`CFSR=0x400`, `HFSR=0x40000000`), which is how SYSMPU
reports: the dumped PC and registers are post-fault and are not the culprit.

**That `# skipped: 2` -> `# skipped: 1` delta is the silicon confirmation of the
`mpu_privileged_guard` deletion.** The previous run on this same board and same service list
recorded two skips, `mutex_deadlock` plus `mpu_privileged_guard` (`reference/boards.md`), and this
one records `mutex_deadlock` alone. **What carries the inference is that BOTH runs NAME their skips
on the wire**, not the totals: the plan count moved for its own reasons (a case was added in the
same milestone), so a count-only comparison would prove nothing. Read the two named transcripts
against each other, never the two numbers.

That also resolves a question M4.5.5 left open, where three skip counts looked mutually
inconsistent. `master` defaults `KICKOS_ROOT_PRIVILEGED=ON`, so the M4.5.5 selftest rows ran
**privileged** and `mpu_privileged_guard` therefore RAN rather than skipping. All three counts
reconcile on that.

It is worth having because a deletion is otherwise the hardest kind of change to witness -- the
evidence for it is an absence. Note the limit of that evidence: the absence of the banner's
`, root unprivileged` suffix proves only that the image carries the new banner code, since the
argument for deleting the suffix is precisely that it carried no information. The suffix witnesses
nothing about posture. The discriminating posture witnesses are `rootfault` on `frdmk64f` -- where
`chip_mk64f.cc` keeps SYSMPU `RGD0` supervisor-`rwx`, so the isolation fault is reachable ONLY from
a user-mode root -- and `rootfault` on `pizero2350`.

For the same reason `c6blink` (ESP32-C6, RISC-V PMP NAPOT) and `rxdrv` (RX72M, RXv3 MPU) each carry
a two-arm possession probe -- negative in `main`, positive as the driver's first act, both printing
rc and the wanted value -- since the positive arm needs a real DEV holder and therefore needs a chip.
**Both have now been run on silicon**, and both pass on both arms:
`[c6blink] PASS periph_enable root rc -1 (want -1)` / `holder rc -38 (want -38)` on
`esp32c6-wroom`, and the identical pair from `[rxdrv]` on `rx72m`. The holder's `-38`
(`-KOS_ENOSYS`) is the positive result: possession PASSED and the call reached a chip layer with no
backend, which is exactly what distinguishes it from the root arm's `-1` (`-KOS_EPERM`) at the gate.

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
captured. **The sim satisfies the memory half of it and only that half**: `KICKOS_HAVE_MPU` is 1
there by arch, `mprotect` really does fault on root's write into a child's region, and the
`rootfault` gate really does pass -- but "unprivileged" on the sim is a narrower region set and
nothing more, so what passes is the kernel's region composition plus the host's enforcement of it,
never a CPU-mode boundary. The distinction matters because the sim arm is now a CI gate and it would
be easy to read its pass as covering more than it does.

**The ring arm is witnessed, on silicon and in CI.** The instrument is
`user/apps/common/ringpriv`, and the silicon witness is `f302nucleo` (2026-07-30, `270b6fa-dirty`):
`CONTROL=0x3` with `nPRIV=1` and `SPSEL=1` both off their reset values, a permitted unprivileged
`msr` to `APSR` moving the read-back, the unprivileged write to `CONTROL.nPRIV` IGNORED with no
self-promotion, and the rest of `CONTROL` unchanged by the ignored write -- `[ringpriv] PASS
(5 arms)`. `f302nucleo` was the right part for it (a real ring, no MPU) and the gap was **missing
code, not a missing board**; that code now exists and the arm is also permanently gated in-env, on
the MPS2 M3/M4/M7/M33 images plus `microbit`, which asserts the OPPOSITE outcome rather than
skipping (`_arms` is 1 there against 5, and `ringppb` is registered only where there IS a ring to
refuse). The emulated measurement below is what established the fault is takeable and what it looks
like; it is not the silicon witness and is not counted as one.

**The authority arm never implies confinement, and must not be written as if it did.** On a board
with no ring, an unprivileged root is real in the kernel's authority checks and absent in the
hardware. Nothing there stops a thread walking past the syscall and touching the peripheral
directly. So an authority-arm pass says the kernel refused to act on the machine on that thread's
behalf, and says nothing about what the thread can reach by itself.

**The confinement arm's ordering is why `f411disco` was witnessed in two separate passes**: its
PMSAv7 enforcement had only ever been build-and-link validated, and the arm has no meaning on a
board where enforcement has never been seen to work, so the enforcement witness came first --
`selftest` `62/62` plus `mpu_fault`, with root still privileged -- and the unprivileged-root witness
second. Both are in `reference/boards.md`. That `62/62` is the suite total **as of that pass** and is
not comparable to today's `1..66`.

**The ring arm is witnessable and cheap, and it is measured under emulation.** On a synthetic
no-MPU unprivileged-root build under QEMU, an unprivileged access to the System Control Space took
`CFSR=0x8200` -- BFSR byte `0x82`, with the **MMFSR byte `0x00`**, so provably not an MPU fault --
and `BFAR=0xE000E280`, which is `NVIC_ICPR0`, inside the SCS. It escalates to HardFault because
nothing in this tree ever sets `SHCSR.BUSFAULTENA`; only `MEMFAULTENA` is ever written
(`arch/arm/common/arch_arm_common.cc:203` and `:268`, `arch/arm/common/arch_arm_pmsav8.cc:99`).

**The prober is built as designed**, in two binaries because one of them ends the process:
`ringpriv` is the survivable register prober that prints its own verdict line, and `ringppb` is the
terminal arm that ends on the privileged-only PPB read. It reuses `mpu_fault`'s shape -- root spawns
an unprivileged child that pokes the register, so the app depends on nothing about root the same way
`mpu_fault` does not. The terminal arm's runner asserts the reporter's marker exactly once plus exit
132, which is how `fault` is wired on every MPS2 image with the `HARD FAULT` marker
(`user/apps/common/fault/CMakeLists.txt:26-29`); the survivable arm has its own runner
(`tests/check_app_arms.sh`), which asserts the `PASS (<n> arms)` verdict and an EXACT arm count,
so an arm cannot be deleted with the gate still green. The register must be `SHCSR`, `ICSR` or
`VTOR` and **not `STIR`**, because `CCR.USERSETMPEND` can legitimately make `STIR`
unprivileged-writable, and this tree writes `STIR` from the kernel already
(`kernel/bench/bench.cc:83`) -- a `STIR` store that lands would prove nothing about the ring.
Classification is per BOARD and enumerated, not defaulted: ARMv7-M always has the ring, `picopi`
(Cortex-M0+) has it, `microbit` (Cortex-M0) does not, and an unclassified armv6m board is a
configure `FATAL_ERROR` rather than a vacuous pass.

**Deleting the knob turned a barely-built gate into an always-built one, which argued for the
deletion on its own.** `user/apps/common/rootfault/CMakeLists.txt` used to register the confinement
gate under `KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND NOT KICKOS_ROOT_PRIVILEGED`. It now registers
under `KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU` alone, so the gate is **always built on six runnable
images**: `sim`, the four MPS2 images (`qemu`, `qemu-m3` and `qemu-m7` on PMSAv7 -- the M7 image with
16 regions -- and `qemu-m33` on PMSAv8), and `qemu-riscv` on RISC-V PMP. **CI runs all six**: the
`sim` job's `ctest --preset sim`, the `qemu-arm` job's PMSA enforcement loop over `qemu qemu-m33
qemu-m7 qemu-m3` at `-DKICKOS_HAVE_MPU=1`, and the `qemu-riscv-mpu` job's enforcement gate.

The claim this record used to make here -- that **no** preset and no CI job configured the flipped
posture -- **was already false before the deletion** and is doubly false now. M4.5.5 added flipped CI
arms, so the gate was running on `qemu` and `qemu-m33` (and on the sim) well before stage 5; the
correction matters because the old wording let the deletion take credit for coverage that already
existed. What the deletion actually bought is narrower and still worth having: `qemu-m3`, `qemu-m7`
and `qemu-riscv` had **no** `rootfault` arm at all, and `qemu_riscv_rootfault` in particular is a
gate CI had **never** run, so RISC-V PMP root confinement went from hand-witnessed to gated.

**And it removed CI work rather than adding it.** The two flipped arms M4.5.5 had added --
`build/sim-flip` in the `sim` job and the `build/$b-flip` loop in `qemu-arm` -- were **deleted**,
because measurement showed `rootfault` was the only test they carried that the base arms lacked, and
the base `KICKOS_HAVE_MPU=1` arms now register it themselves. A duplicate configure-and-build of the
same posture is pure wall-clock cost once the posture is the only one there is. `rootauth`'s flipped
arm folded in the same way.

`rootauth` also **gained a `microbit` arm** (`microbit_rootauth`), which is worth explaining because
that board has no privilege ring at all (section 2). It works precisely because `Thread::privileged`
is a **software field**: `cap_check_authority` reads it and stops short-circuiting on it once root is
unprivileged, whether or not the CPU honours the bit. So the authority arm is fully witnessable on a
board where the ring is inert, and armv6m -- which already had a `ctest` run gate but no authority
gate -- now carries one. It witnesses nothing about the ring, exactly as the top of this section
says.

**`esp32-wroom` could not be given a run gate.** Upstream QEMU has no ESP32 machine, so there is no
board-to-machine mapping to construct and no way to run the image at all. The LX6 therefore keeps
build-only coverage for the authority arm despite that arm needing no hardware -- a **remaining
gap**, and one that needs an emulator rather than a board or an app.

**`pizero2350` owes `rootfault` and `rootauth`.** Both were attempted 2026-07-30 and neither
produced a capture: the board left the USB bus (KickOS has no USB device stack) and needs a physical
BOOTSEL press to reflash. Its `selftest` under enforcement did land in the same session (`1..66`,
`# skipped: 0`, `# all tests passed`), so PMSAv8 is not in question; what is owed is the two
confinement/authority gates specifically.

**What nothing witnesses.** Two coverage gaps here are structurally unfillable by emulation:
v6-M MPU programming (QEMU models no Cortex-M0+ and no Cortex-M23 core) and the M7 speculation
class. Neither can be closed by adding a gate; both stay silicon-proven or unproven.
