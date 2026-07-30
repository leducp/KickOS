<!-- SPDX-License-Identifier: CECILL-C -->
# Unprivileged root -- start unprivileged holding capabilities

> **Status: ACTIVE** -- stages 0 through **3** have landed. Six boards boot an unprivileged root
> and are witnessed on silicon, covering every enforcement backend -- `xmc4800-relax` (PMSAv7),
> `esp32c6-wroom` (RISC-V PMP), `pizero2350` (PMSAv8), `rx72m` (RXv3), `f411disco` (PMSAv7) and
> `frdmk64f` (SYSMPU), which is the first board to run its **full** service list under the flip
> (console `k64uart` + SPI `k64dspi`) rather than a console-only one. The per-board captures are in
> `reference/boards.md`. Stages 4 and 5 remain; the `KICKOS_ROOT_PRIVILEGED` deletion is stage 5 and
> is no longer order-blocked. See `design/README.md` for the marker taxonomy. The actionable
> checklist is `TODO.md`; this record is the reasoning behind it, which `TODO.md` does not carry.

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
7. Stage plan, and what stages 0 to 3 landed
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

**The deletion was order-blocked behind stage 3, and is not any more.** The block was mechanical:
`frdmk64f` under `KICKOS_HAVE_MPU` defaults to the `kickos_services_frdmk64f` service list, and
while that list's bring-up wrote MMIO from root it sat in `KICKOS_SERVICE_LIST_ROOT_MMIO`, whose
configure-time `FATAL_ERROR` refuses an unprivileged root (`CMakeLists.txt`). With the knob gone
that refusal has no posture left to be conditional on, so it would fire unconditionally and the
board's enforcement build would **stop configuring** -- the `build-boards-mpu` CI job, not a bench
inconvenience. Stage 3 moved those writes into the drivers and the list came off the refusal list.

What remains on it is `kickos_services_xmc4800relax`, the combined XMC console+SPI list, because
`xmcssc` still configures the USIC from root and needs a seam stage 3 does not provide (section 9).
No board selects it in the unprivileged posture, so the deletion's job there is to make the
console-only list that board's unconditional default rather than a posture-selected one; section 7
carries that and the other build consequence.

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
`AUTH_PINMUX` with `AUTH_CLOCK`" plan the old ceiling forced.

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

**`AUTH_DEVICE` in its old shape is rejected.** It meant "console publish, shutdown, reboot",
which holds together only while root does all three. Once root is *only* a spawner, publishing a
console and ending the system belong to **different** threads, so no single bit can carry both
without handing the console driver the power to end the system.

The cut is six bits:

| Bit | Gates | Who holds it |
|---|---|---|
| `AUTH_MEMORY` | `ram_alloc`, the MMIO window grant, `mem_self_grant` | root, the spawner |
| `AUTH_PINMUX` | `pinmux_set` | root, for the board pin map; plus an app muxing its own pins |
| `AUTH_PSTATE` | `cpu_clock_set` | a CPU-governor service, and nothing else |
| `AUTH_IRQ` | `irq_attach`, `irq_unmask` | drivers with lines |
| `AUTH_SYSTEM` | `shutdown`, `reboot` | root / init |
| `AUTH_CONSOLE` | `console_publish` | root, during service bring-up |

`AUTH_DEVICE` and `AUTH_CLOCK` cease to exist as names. **`cpu_clock_set` keeps a bit of its own**
precisely because a governor service needs clock-rate authority and nothing else: folding it into
the lifecycle bit would hand the governor the power to end the system.

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
`kickos_app_authority()`, whose weak default is `AUTH_MEMORY | AUTH_SYSTEM`: spawn worker threads,
and end the system when `main` returns. An app needing more states it in its own translation unit
with `KICKOS_APP_AUTHORITY`, which is per-*executable* -- the alternative, a CMake variable, is one
value per build tree, and a single tree links `selftest` and `stress` against the same kernel.

An app whose `main` returns must keep `AUTH_SYSTEM`: `root_entry` ends the system with
`kos_shutdown`, and a refused shutdown reaches `KICKOS_UNREACHABLE("root: shutdown refused")`,
which panics with that text on the reclaimed UART. The bit is therefore *not* forced back on --
the failure is already legible, and forcing it would deny a never-returning app the ability to
declare 0 and hold nothing at all.

**None of this has any effect where root is privileged**, because `cap_check_authority` returns
true on `Thread::privileged` before it reads the cap at all, and `kmain` seats root a cap only
under `KICKOS_ROOT_PRIVILEGED=OFF`. On a privileged-root board the narrow finds an empty slot and
answers `-KOS_EBADF`, which the init tolerates by design. The consequence worth stating plainly:
**this confines an app only on the boards that have flipped**, and it is silently inert everywhere
else until the knob is deleted.

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
is, which would make the flip a memory-isolation change and nothing more.

## 7. Stage plan, and what stages 0 to 3 landed

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
deleted (section 4). Two consequences sit in the build rather than in the kernel: `xmc4800-relax`
stops picking between two service lists by posture and always takes the console-only one, and the
confinement gate stops being opt-in (section 10).

`f411disco` came last because what stood in the way was a **pre-existing bench debt rather than
flip work**: PMSAv7 enforcement had never been witnessed on that board at all, so a flip there
would have had no enforcing baseline to be discriminating against.

**Stage 3 -- `arch_periph_enable(base)`. LANDED.** `int arch_periph_enable(uintptr_t base)`
(`arch/include/kickos/arch/arch.h`) ungates the block's clock and drops its bus-side
supervisor-protect, with a weak `-KOS_ENOSYS` default in `kernel/time/clock_select.cc`. Syscall
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
the old `AUTH_DEVICE` before it -- would hand `kos_shutdown` and reboot-to-bootloader to every
unprivileged driver in the fleet; every other bit carries collateral just as unwanted, and a bit of
its own would have to be justified against the same possession argument that removes the need for
one. Possession is *sufficient* because a granted window already
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
  unprivileged, which turns `sim_stress` from FAIL to PASS under `KICKOS_ROOT_PRIVILEGED=OFF`; the
  old flags were refused `-KOS_EPERM` there.
- **The in-env witness is the selftest's `authority_cap`**, whose worker drops its only authority and
  is then refused by the gate that had just answered for it. The *root* narrow has no in-env carrier
  for the same reason section 10 gives elsewhere, so it was witnessed by removing `AUTH_CONSOLE`
  from `initdemo`'s declaration: `console_publish` then fails from root on `qemu` at
  `KICKOS_ROOT_PRIVILEGED=OFF`, and the identical source passes at `ON`.

**Stage 5 -- delete `KICKOS_ROOT_PRIVILEGED`.** The knob goes with no replacement and no porting
escape hatch; section 4 carries the decision and its reasons. Three things travel with the deletion:
`ThreadAttr::privileged` becomes `false` (`kernel/include/kickos/thread.h:164`), the invariant that
leaves is **"exactly one privileged thread, and it is `idle`"**, and every remaining `#if
KICKOS_ROOT_PRIVILEGED` site loses its condition rather than its body.

**Its one ordering constraint is discharged.** With the knob gone the root-MMIO service-list
`FATAL_ERROR` has no posture left to be conditional on and fires unconditionally, which would have
stopped `frdmk64f`'s enforcement build from configuring at all -- the `build-boards-mpu` CI job.
Stage 3 retired that board's root MMIO writes and its service list came off the refusal list. The
list still holds `kickos_services_xmc4800relax`, so the deletion has to make the console-only list
`xmc4800-relax`'s unconditional default at the same time. Section 4 states the same, because whoever
schedules the deletion needs it there.

**The payoff is already established** (section 10): `user/apps/common/rootfault/CMakeLists.txt:18`
registers the confinement gate under `KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND NOT
KICKOS_ROOT_PRIVILEGED`, and nothing in the tree configures that posture. With the third term gone
the gate is unconditional wherever `KICKOS_HAVE_MPU`, which turns a never-built gate into an
always-built one on five runnable QEMU images: `qemu`, `qemu-m33`, `qemu-m7`, `qemu-m3` and
`qemu-riscv`.

`selftest` was on that list and **came off it**. Its only two privileged spawns were in
`rr_interleave`, and they turned out not to be a requirement at all -- the test wants two
threads interleaving under round-robin, which privilege has nothing to do with -- so they were
made unprivileged rather than pinning the whole suite to a posture. That matters beyond the one
app: selftest is the thing that *witnesses* the flip, so leaving it privileged-root would have
made the boundary untestable on exactly the boards that adopt it. It now runs in both postures
(59/60 flipped, `mpu_privileged_guard` skipping because its subject is the privileged posture).

## 8. The three blockers

**One service bring-up body still pokes MMIO directly from root**, and it is
`system/driver/xmc4800/xmcssc/xmcssc.cc` (USIC kernel clock, baud, protocol) on `xmc4800-relax`, the
enforcement flagship. The two K64F bodies are retired:
`system/driver/mk64f/k64uart/k64uart.cc` (AIPS PACR) and
`system/driver/mk64f/k64dspi/k64dspi.cc` (clock gates, pin mux, GPIO, DSPI config) each call
`arch_periph_enable` from the driver thread that holds the window, which is why `frdmk64f` runs its
full service list with root writing no MMIO at all.

**From root** was the operative phrase for those two. On the XMC it is only most of the story:
section 9 measures that three of the registers are refused to an unprivileged thread even inside
the granted window, so that bring-up needs both a move into the driver thread and a kernel seam
for those three -- a USIC-shaped seam that stage 3's "ungate a clock, drop supervisor-protect" does
not provide. That is why `xmc4800-relax` stays console-only under the flip.

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
  omission**, which is what separates its consequence from `f411spi`'s: `k64drv` cannot run under
  the flip *by design*, where `f411spi` was merely waiting for a seam.

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
  `INPR`'s RINP/AINP onto SR0, the kernel console's node, and drives the in-kernel
  `console_tx_isr` at **~37,700 invocations/second**. The honest severity is a **bounded parasitic
  CPU tax, not a denial of service** -- the console kept beating and the board did not wedge. The
  reason is that the USIC receive service request is **edge** (one pulse per received word), so a
  held, uncleared flag does not re-assert the node; the same probe confirmed it by leaving RIF/AIF
  set with the attacker idle, which took zero further ISR entries. The tax therefore lasts exactly
  as long as the attacker runs, and the probe pins the attacker below root's priority so a wedge
  could not have been mistaken for starvation.
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
  The two boards are otherwise not one case: `bluepill-c8` carries its own `board_config.h`
  (`boards/bluepill-c8/`), while `f302nucleo` has no board directory at all and takes the chip's.
  Both take their chip's linker script.
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
bootloader entry declines through the seam's weak `-KOS_ENOSYS` default rather than pretending.

**Decision: it shares `arch_shutdown`'s authority**, rather than taking a `CAP_REBOOT` at index 3
or a rights bit of its own. Two reasons: reboot-to-bootloader is the same class of act as shutdown
(end this system, hand the chip to something else); and index 3 is the last free well-known index,
worth more than bit granularity here. That shared bit is `AUTH_SYSTEM` (section 5.1).

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
2. **The weak `-KOS_ENOSYS` decline becomes per-MODE, not per-function.** A chip that can reset but
   has no documented bootloader entry declines both today. The asymmetry that motivates the split is
   **ARM-specific, and architectural rather than measured**: `SCB->AIRCR` `SYSRESETREQ` is available
   on armv6m/v7m/v8m with no chip-specific code, so a normal reset could sit in `arch/arm/common`,
   whereas bootloader entry is per-chip and only three backends have one (rp2040, rp2350,
   imxrt1062). RISC-V has no architectural reset, and RX and Xtensa each have their own mechanism.
3. **The knob gates only the bootloader mode, and is renamed
   `KICKOS_ENABLE_REBOOT_TO_BOOTLOADER`.** `KICKOS_ENABLE_SELFTEST` conflates "test-only syscall
   surface" with "may this image put the board into firmware-accept mode", and the conflation costs
   a real capability for no security gain: no production in-kernel path can reset the chip at all,
   while a privileged thread may need to sequence one -- watchdog recovery, a fault-handler reset, a
   bring-up retry -- none of which carries the bootloader's risk. **Not `..._IN_FLASH_MODE`**: the
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
  gates, which would otherwise ship unexercised until a board flips -- the same vacuity trap
  `kernel_ctor_placement` fell into.

**Stage 3's possession gate has a negative arm in the suite and a positive arm only on hardware.**
`selftest`'s `periph_enable_unheld` asserts the refusal of a caller holding no window, and that runs
everywhere. The positive arm has **no in-env carrier at all**, because the host sim can never hold a
DEV region: `arch_mpu_region_encodable` returns false unconditionally (`arch/sim/sim.cc`), so the
grant that would carry the window cannot exist there. Driver bring-up on silicon is what witnesses
it instead. On `frdmk64f`, an unprivileged root with the **full** service list (`k64uart` +
`k64dspi`) runs `selftest` `1..65` `# all tests passed (2 skipped)` -- the skips being
`mpu_privileged_guard`, whose subject is the privileged posture, and the pre-existing
`mutex_deadlock # SKIP pool too small` -- and `rootfault` denies root's cross-domain write with
`SYSMPU ISOLATION FAULT: port=3 addr=0x2001a000 master=0 W EDR=0x80000003`, `CFSR=0x400`,
`HFSR=0x40000000`. The captures are in `reference/boards.md`.

For the same reason `c6blink` (ESP32-C6, RISC-V PMP NAPOT) and `rxdrv` (RX72M, RXv3 MPU) each carry
a two-arm possession probe -- negative in `main`, positive as the driver's first act, both printing
rc and the wanted value -- since the positive arm needs a real DEV holder and therefore needs a chip.
Neither has been run on silicon yet.

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
