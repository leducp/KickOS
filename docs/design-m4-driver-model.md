<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# M4 driver model -- the class/service duality

> **Status: LANDED** -- the packaging RULING is settled and the driver libs under
> `system/driver/<chip>/` follow it; the framework it describes shipped in M4.8.1,
> `design-generic-driver-service.md`.
> The ruling itself now has a Reference home: `reference/architecture.md`, *Driver packaging: class
> versus service*, which is code-synced and is what a reader should cite. This document is kept as
> the decision record and for the numbered rules below, which source comments and sibling design
> records cite BY NUMBER -- do not renumber them.

Records the M4 decision on HOW a driver is packaged: as an in-process CLASS (driver-lib), as a
shared SERVICE (a thread behind an endpoint), or -- the ruling -- as BOTH, with the service composed
on top of the class. Builds on the landed cap table + CAP_ENDPOINT; does not reopen them. Companion
to `design-driver-era-scope.md` (M4 gap list) and `design-m4-driver-matrix.md` (which peripherals,
weighted).

## The ruling

**The class is the primitive; the service is a thin thread composed on top of it. Never the
reverse.** The class is written first and defines the API; the service is a transport over that same
API and is not allowed to invent its own. Full statement, both capability shapes, and the
bus-versus-device split: `reference/architecture.md`, *Driver packaging: class versus service*.

## Why this is the microkernel dividend

Because drivers live in userspace, the CONSUMER -- not the kernel -- chooses the coupling and pays
only for what it uses. A consumer that cannot afford an IPC round-trip links the class and calls it
inline (lowest latency, single owner, zero kernel tax); a consumer that wants sharing talks to the
service and pays the round-trip deliberately, in exchange for arbitration and isolation from the
device. The kernel levies no driver tax at all -- it only routes capabilities -- so the same
peripheral is a private inline class in one image and a shared service in another with no kernel
change. A kernel-resident driver cannot offer that choice, which is why putting drivers in
userspace is what makes the duality possible. This is the clearest single demonstration of the
microkernel structure in KickOS, and it is the reason the ruling reads the way it does.

## The 1:1 rule (what stops it rotting)

The service request protocol MUST be a 1:1 serialization of the class methods -- the endpoint is
literally "the class API, over the wire." If the two drift, two APIs must be maintained and the
agnostic contract erodes. The class is THE contract; the service is a transport. A new capability
on the class is a new message on the service, mechanically.

## What the class API must NOT assume

- It does not own a thread. It runs in the caller's thread (inline) or the service's thread
  (shared) -- it must work either way.
- It does not own an endpoint. It is a synchronous transaction primitive; any blocking is the
  caller's wait, not an IPC it initiates.
- It does not assume exclusivity beyond the peripheral cap it was handed.

These are what make the same object usable both inline and behind a service.

## Watchdog: class by default

A watchdog is single-owner liveness proof, so it is a class instantiated in the thread that must
prove it is alive; the kick authority is the cap on its MMIO region. Routing the kick through a
service would INVERT its purpose -- if the service thread wedged, every client would fail to kick,
which is the exact failure a watchdog exists to catch. The one service case is a software-watchdog
SUPERVISOR: N threads check in, the supervisor owns the watchdog class instance and kicks the
hardware only if all checked in, handing out per-client check-in caps. Still built on the class.

## Sensors: the same duality

A platform SENSOR SERVICE aggregates every sensor on the board -- discovery, naming, sharing one
reading across many clients. A consumer that wants only ONE sensor and shares its reading with no
one instantiates the sensor CLASS directly, inline, with no dependency on the service thread. The
die-temperature sensor (present on all four M4 matrix boards; see `design-m4-driver-matrix.md`) is
the canonical example: cheap to read, on every board, fits no existing API, and demonstrates both
ends of the duality.

## Ownership and the class-driver core

The duality above answers HOW a driver is packaged. This section answers the prior questions: WHO
owns each peripheral, whether the kernel ever hands it off, and where the shared register logic
physically lives so both trust domains can link it. These rules REFINE the duality; they do not
replace it.

### 1. Resources are single-owner, whole-block

Each peripheral instance has exactly one owner at any instant, and a peripheral is never
concurrently shared between the kernel and userspace. Ownership is at BLOCK granularity, not
per-channel: the MPU grants at block/page granularity, so a timer block is entirely the kernel's or
entirely a userspace driver's. You do not split one block's channels across owners -- the grant
mechanism cannot express it, and single-writer would no longer be free. This is the same
single-owner property the class model relies on (the MMIO grant IS the authority), stated as a
whole-block invariant across the boot lifetime rather than only at spawn.

### 2. Three ownership models, keyed on kernel stake

Decided by one question: does the kernel have ongoing state or stake in the resource?

- **Owns-for-life** -- timebase, MPU, IRQ controller. Never leaves the kernel. They sit on the hot
  path, back the TCB, and are circularly dependent on the scheduler itself (a userspace timebase
  driver would be a thread the scheduler must schedule using the timebase it is trying to provide).
  Not a driver candidate at all.
- **Neutralize-then-grant** -- watchdog. The kernel touches it ONCE at boot, only to defuse a
  ROM-armed hazard, then holds ZERO live state and ZERO reclaim interest: it stops touching the
  block and grants the window to whoever wants it. There is NO handover protocol. If a userspace
  owner later re-enables it and then dies without kicking, the board resets -- a correct watchdog
  outcome, not a kernel concern.
- **Stateful handover** -- console. The kernel ACTIVELY uses it (boot banner, panic output, the TX
  ring and its drain ISR) and wants it BACK on panic. The ONLY case that needs the handover
  primitive: quiesce, transfer, reclaim. It is the M3 ConsoleState machine generalized to any
  resource the kernel both lends out and must recover.

Restated: no ongoing stake -> neutralize-then-grant (or never touch it); stake, and reclaims on
panic -> stateful handover; stake forever -> owns-for-life.

### 3. Class driver = shared freestanding code (the DRY core)

Where two owners (the kernel and a userspace driver) run the same register logic, that logic is
factored into a CLASS DRIVER written to the KERNEL BAR -- the same constraints the kernel links
under: no constructors or destructors (the kernel routes app and library constructors out to
`root_entry` and keeps only its own minimal `.init_array`, and destructors never run because the
kernel never exits), no exceptions, no STL, POD state plus free functions, explicit `init` and never
implicit lifetime.

Such a leaf is linkable from BOTH the TCB and unprivileged userspace unchanged. Full-C++ and RAII
ergonomics live ONLY in the userspace service wrapper (and the userspace inline-class wrapper),
never in code the kernel links. **The class/service boundary IS the ctor-freedom boundary**, which
is consistent with the 1:1 rule: the freestanding class is the contract, the RAII service is a
transport and an ergonomic skin over it. Proof obligation: the kernel's own arch/chip timebase
becomes consumer #1 of this layer, demonstrating that the freestanding core really does serve the
kernel bar and not just userspace.

### 4. Class-driver API convention: explicit context, not an internal index

The class driver takes the instance's context EXPLICITLY -- a base address, or a small POD
descriptor passed by pointer -- never an internal instance-index table. Write
`wdog_disable(uintptr_t base)`, not `wdog_disable(0)`. Three reasons:

- Stateless: no static mutable globals, which is what keeps the leaf kernel-bar clean (rule 3).
- It maps onto single-ownership (rule 1). The owner holds the descriptor for ITS instance; a
  userspace service granted one instance is handed that base and structurally cannot name the
  others. An index namespace, by contrast, is global and cannot be granted per-instance -- index 2
  is nameable by anyone who can spell "2".
- It does not bake board configuration (which instances exist, how many) into driver logic.

This is the "object in C" form: a POD struct plus free functions taking it by pointer -- no ctor, no
dtor, no vtable. A stateless op takes just the base; a stateful op (enable-plus-kick with a timeout)
takes a small POD descriptor. General rule: **N identical instances -> parameterize by base; a
different register layout -> a different function.**

### 5. Boot-critical vs late-bindable

- **Boot-critical** -- timebase, watchdog. ROM-armed and able to reset-loop before userspace even
  exists, so they need kernel-first-touch EARLY in `arch_init`.
- **Late-bindable** -- GPIO, SPI, and the rest. No boot hazard; can wait for a userspace driver
  brought up on demand.

The ownership model (rule 2) and the binding time (rule 5) are INDEPENDENT axes: the watchdog is
neutralize-then-grant AND boot-critical; a late GPIO service is owns-nothing-special AND
late-bindable.

### 6. The build-layering decision M4 must nail

WHERE the class-driver leaf physically lives so that BOTH the TCB and an unprivileged userspace
driver can link it -- a single leaf consumed from two trust domains. This is a build-graph and
source-tree placement question, not an API question; the API is fixed by rules 3 and 4. It is the
concrete deliverable that makes the DRY core real.

### 7. Enforcing single-ownership: the grant refuses kernel-reserved blocks

**LANDED, and fully specified elsewhere.** Rule 1 is only real if the kernel can REFUSE a grant that
overlaps a resource it owns, which turns rule 1 from "trust the granter" into "the kernel refuses".
The mechanism, the predicate, the chokepoint, the boot-time validation and the no-fallback-TU
`arch_reserved_blocks` requirement are the contract `reference/invariants.md`
(`grant-refuses-kernel-reserved-blocks`) states in full, summarised in
`reference/architecture.md` under *Rule 7*. This heading is kept because source comments
(`arch/include/kickos/arch/arch.h`, `kernel/include/kickos/grant.h`) and sibling design records cite
`sec.7` by number; the text that used to be here duplicated the invariant verbatim and was deleted
rather than left to drift.

One point worth keeping here because it belongs to this document's subject: rule 7 is ORTHOGONAL to
rule 3's DRY code reuse. Sharing the class-driver register logic is a LINK-time decision; refusing
the resource grant is a RUNTIME capability decision. The kernel-owned timer INSTANCE is never
granted even though its register CODE is shared.

### Worked example: the RT1062 watchdogs

`arch/arm/chip/imxrt1062/chip_imxrt1062.cc`'s `watchdog_disable()` is the illustration. It handles
WDOG1/2 (the 16-bit WMCR power-down family) and the RTWDOG (WDOG3: unlock key, CS/TOVAL, and the
CS.RCS reconfig-success confirm with a retry), and it is ALREADY kernel-bar-clean: free functions,
constexpr register addresses, an IRQ-masked reconfig window, no ctor/dtor.

In M4 this lifts, unchanged in spirit, into a per-chip class-driver leaf (e.g.
`imxrt1062/wdog.{h,cc}`) taking base explicitly:

```
void     wdog_disable(uintptr_t base);              // WMCR family: WDOG1, WDOG2
void     rtwdog_disable(uintptr_t base);            // distinct RTWDOG type
void     rtwdog_enable(uintptr_t base, uint32_t timeout);
void     rtwdog_kick(uintptr_t base);
```

- Ownership model: neutralize-then-grant. Chip `arch_init` calls the disables early (boot-critical,
  rule 5). A userspace watchdog service can later be granted the RTWDOG block and use
  `rtwdog_enable` / `rtwdog_kick`, packaged per *Watchdog: class by default* above.
- The API mirrors the silicon's TYPE structure, not a flat index. The three are NOT identical:
  WDOG1 and WDOG2 are one register family and parameterize by base; the RTWDOG is a distinct type
  and gets its own functions. Rule 4 applied.

Do NOT refactor the code for this now: the routine is single-use today, and lifting it into the
shared leaf is the M4 target.
