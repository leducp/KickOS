<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# M4 driver model -- the class/service duality

Terse, invariant-first. Records the M4 decision on HOW a driver is packaged:
as an in-process CLASS (driver-lib), as a shared SERVICE (a thread behind an
endpoint), or -- the ruling -- as BOTH, with the service composed on top of the
class. Builds on the landed cap table + CAP_ENDPOINT; does not reopen them.
Companion to `design-driver-era-scope.md` (M4 gap list) and
`design-m4-driver-matrix.md` (which peripherals, weighted).

## The ruling

**The class is the primitive; the service is a thin thread composed on top of
it. Never the reverse.**

- **Class (driver-lib).** A hardware-agnostic object linked directly into the
  consumer thread. No IPC, no thread of its own, no endpoint. The transaction
  runs inline, in the caller's thread. A tight-coupling consumer (KickCAT ESC in
  its cyclic fieldbus loop) uses this and pays nothing beyond the register work.
- **Service.** A thread that owns exactly one class instance plus the peripheral
  capability, and multiplexes unrelated clients over a badged CAP_ENDPOINT
  (call/reply, reply-cap). Sharing and arbitration live here, and ONLY here.

The class is written first and defines the API. The service is a transport over
that same API -- it is not allowed to invent its own.

## Why this is the microkernel dividend

Because drivers live in userspace, the CONSUMER -- not the kernel -- chooses the
coupling, and pays only for what it uses:

- A consumer that cannot afford (or does not want) an IPC round-trip links the
  class and calls it inline. Lowest latency, single owner, zero kernel tax.
- A consumer that wants sharing talks to the service and pays the round-trip
  deliberately, in exchange for arbitration and isolation from the device.

The kernel levies no driver tax at all -- it only routes capabilities. The same
peripheral can be a private inline class in one image and a shared service in
another, with no kernel change. A kernel-resident driver cannot offer this
choice; putting drivers in userspace is what makes the duality possible. This is
the clearest single demonstration of the microkernel structure in KickOS.

## Two capability shapes, both first-class

- **Class model -- the cap IS the hardware.** The MMIO region granted at spawn
  (`kos_thread_params` mmio grant). "You hold the device." Holding the region is
  the authority; the cap system enforces single-writer for free.
- **Service model -- the cap is a badged endpoint.** "You may ASK the holder."
  The client never touches the MMIO; it holds only the right to send requests.

Neither is the "real" one. A driver may be reached both ways in different images.

## The 1:1 rule (what stops it rotting)

The service request protocol MUST be a 1:1 serialization of the class methods --
the endpoint is literally "the class API, over the wire." If the two drift, two
APIs must be maintained and the agnostic contract erodes. The class is THE
contract; the service is a transport. A new capability on the class is a new
message on the service, mechanically.

## What the class API must NOT assume

- It does not own a thread. It runs in the caller's thread (inline) or the
  service's thread (shared) -- it must work either way.
- It does not own an endpoint. It is a synchronous transaction primitive; any
  blocking is the caller's wait, not an IPC it initiates.
- It does not assume exclusivity beyond the peripheral cap it was handed.

These are what make the same object usable both inline and behind a service.

## Bus vs device (the SPI shape)

A shared bus needs a second split inside the class layer:

- **Bus class** -- owns the peripheral + its cap; owns the transaction engine.
- **Device handle** -- chip-select + per-device config; issued against a bus.

KickCAT ESC holds its OWN bus instance when it is the sole user of that SPI
peripheral (fully inline, no arbitration). When unrelated clients share the bus,
a bus SERVICE owns the bus class and arbitrates device-handle traffic. The
one-block-many-modes parts (XMC USIC, RX SCI = UART/SPI/I2C by mode; see
`design-m4-driver-matrix.md`) resolve the mode-select seam IN THE CLASS, not the
service.

## Watchdog: class by default

A watchdog is single-owner liveness proof, so it is a class instantiated in the
thread that must prove it is alive; the kick authority is the cap on its MMIO
region. Routing the kick through a service would INVERT its purpose -- if the
service thread wedged, every client would fail to kick, which is the exact
failure a watchdog exists to catch. The one service case is a software-watchdog
SUPERVISOR: N threads check in, the supervisor owns the watchdog class instance
and kicks the hardware only if all checked in, handing out per-client check-in
caps. Still built on the class.

## Sensors: the same duality

A platform SENSOR SERVICE aggregates every sensor on the board -- discovery,
naming, sharing one reading across many clients. A consumer that wants only ONE
sensor and shares its reading with no one instantiates the sensor CLASS directly,
inline, with no dependency on the service thread. The die-temperature sensor
(present on all four M4 matrix boards; see `design-m4-driver-matrix.md`) is the
canonical example: cheap to read, on every board, fits no existing API, and
demonstrates both ends of the duality.

## Multi-instance

Multi-instance = thread-per-instance (one service thread per bus/device it
fronts), consistent with `design-driver-era-scope.md`.

## Ownership and the class-driver core

The duality above answers HOW a driver is packaged (inline class vs shared
service). This section answers the prior questions: WHO owns each peripheral,
whether the kernel ever hands it off, and where the shared register logic
physically lives so both trust domains can link it. These rules refine the
duality; they do not replace it -- the class is still the primitive, the service
is still the thin thread on top.

### 1. Resources are single-owner, whole-block

Each peripheral instance has exactly one owner at any instant. A peripheral is
never concurrently shared between the kernel and userspace. Ownership is at BLOCK
granularity, not per-channel: the MPU grants at block/page granularity (Book
ch.3.7), so a timer block is entirely the kernel's or entirely a userspace
driver's. You do not split the channels of one block across owners -- the grant
mechanism cannot express it, and single-writer would no longer be free.

This is the same single-owner property the class model relies on (the MMIO grant
IS the authority); here it is stated as a whole-block invariant across the boot
lifetime, not just at spawn.

### 2. Three ownership models, keyed on kernel stake

The model for a given peripheral is decided by one question: does the kernel have
ongoing state or stake in the resource?

- **Owns-for-life** -- timebase, MPU, IRQ controller. Never leaves the kernel.
  These sit on the hot path, back the TCB, and are circularly dependent on the
  scheduler itself (a userspace timebase driver would be a thread the scheduler
  must schedule using the timebase it is trying to provide). Not a driver
  candidate at all.
- **Neutralize-then-grant** -- watchdog. The kernel touches it ONCE at boot, only
  to defuse a ROM-armed hazard, then holds ZERO live state and ZERO reclaim
  interest. It simply stops touching the block and grants the MMIO window to
  whoever wants it. There is NO handover protocol. If a userspace owner later
  re-enables it and then dies without kicking, the board resets -- that is a
  correct watchdog outcome, not a kernel concern.
- **Stateful handover** -- console. The kernel ACTIVELY uses it (boot banner,
  panic output, the TX ring plus its drain ISR) and wants it BACK on panic. This
  is the ONLY case that needs the handover primitive: quiesce, transfer, reclaim.
  It is the M3 ConsoleState machine (KERNEL_OWNED -> USER_OWNED -> RECLAIMED),
  generalized to any resource the kernel both lends out and must recover.

The decider, restated:

- No ongoing stake -> neutralize-then-grant (or never touch it at all).
- Stake, and reclaims on panic -> stateful handover.
- Stake forever -> owns-for-life.

### 3. Class driver = shared freestanding code (the DRY core)

Where two owners (kernel and a userspace driver) run the same register logic,
that logic is factored into a CLASS DRIVER written to the KERNEL BAR -- the same
constraints the kernel links under:

- No constructors or destructors. The kernel routes app and library constructors
  out to `root_entry` and keeps only its own minimal `.init_array`; a shared
  class driver must add ZERO static objects that need construction. Destructors
  never run -- the kernel never exits.
- No exceptions, no STL.
- POD state plus free functions. Explicit `init`, never implicit lifetime.

Such a leaf is linkable from BOTH the TCB and unprivileged userspace unchanged.
Full-C++ and RAII ergonomics -- constructors, RAII-owning handles, STL -- live
ONLY in the userspace SERVICE wrapper (and the userspace inline-class wrapper),
never in code the kernel links. The class/service boundary IS the ctor-freedom
boundary. This is consistent with the 1:1 rule above: the freestanding class is
the contract; the RAII service is a transport and an ergonomic skin over it.

Proof obligation: the kernel's own arch/chip timebase becomes consumer #1 of
this layer, demonstrating that the freestanding core really does serve the
kernel bar, not just userspace.

### 4. Class-driver API convention: explicit context, not an internal index

The class driver takes the instance's context EXPLICITLY -- a base address, or a
small POD descriptor passed by pointer -- never an internal instance-index table.
Write `wdog_disable(uintptr_t base)`, not `wdog_disable(0)`. Three reasons:

- Stateless: no static mutable globals, which is what keeps the leaf kernel-bar
  clean (rule 3).
- It maps onto single-ownership (rule 1). The owner holds the descriptor for ITS
  instance. A userspace service granted one instance is handed that base and
  structurally cannot name the others. An index namespace, by contrast, is global
  and cannot be granted per-instance -- index 2 is nameable by anyone who can
  spell "2".
- It does not bake board configuration (which instances exist, how many) into
  driver logic.

This is the "object in C" form: a POD struct plus free functions taking it by
pointer -- no ctor, no dtor, no vtable. A stateless op takes just the base; a
stateful op (enable-plus-kick with a timeout) takes a small POD descriptor.
General rule: N identical instances -> parameterize by base; a different register
layout -> a different function.

### 5. Boot-critical vs late-bindable

- **Boot-critical** -- timebase, watchdog. ROM-armed and able to reset-loop
  before userspace even exists, so they need kernel-first-touch EARLY in
  `arch_init`.
- **Late-bindable** -- GPIO, SPI, and the rest. No boot hazard; can wait for a
  userspace driver brought up on demand.

The ownership model (rule 2) and the binding time (rule 5) are independent axes:
the watchdog is neutralize-then-grant AND boot-critical; a late GPIO service is
owns-nothing-special AND late-bindable.

### 6. The build-layering decision M4 must nail

The open decision M4 has to settle: WHERE the class-driver leaf module physically
lives so that BOTH the TCB and an unprivileged userspace driver can link it -- a
single leaf consumed from two trust domains. This is a build-graph and
source-tree placement question, not an API question; the API is fixed by rules 3
and 4 above. It is the concrete deliverable that makes the DRY core real.

### Worked example: the RT1062 watchdogs

The current `arch/arm/chip/imxrt1062/chip_imxrt1062.cc` `watchdog_disable()` is
the illustration. It handles WDOG1/2 (the 16-bit WMCR power-down family) and the
RTWDOG (WDOG3: unlock key, CS/TOVAL, and the CS.RCS reconfig-success confirm with
a retry). It is ALREADY kernel-bar-clean: free functions, constexpr register
addresses, an IRQ-masked reconfig window, no ctor/dtor.

In M4 this lifts, unchanged in spirit, into a per-chip class-driver leaf (e.g.
`imxrt1062/wdog.{h,cc}`) exposing, taking base explicitly:

```
void     wdog_disable(uintptr_t base);              // WMCR family: WDOG1, WDOG2
void     rtwdog_disable(uintptr_t base);            // distinct RTWDOG type
void     rtwdog_enable(uintptr_t base, uint32_t timeout);
void     rtwdog_kick(uintptr_t base);
```

- Ownership model: neutralize-then-grant. Chip `arch_init` calls the disables
  early (boot-critical, rule 5): `wdog_disable(WDOG1_BASE)`,
  `wdog_disable(WDOG2_BASE)`, `rtwdog_disable(RTWDOG_BASE)`. A userspace watchdog
  service can later be granted the RTWDOG block and use `rtwdog_enable` /
  `rtwdog_kick` (packaged per "Watchdog: class by default" above -- the class
  instantiated in the thread that proves liveness).
- API mirrors the silicon's type structure, not a flat index. The three are NOT
  identical: WDOG1 and WDOG2 are one register family, so they parameterize by
  base; the RTWDOG is a distinct type and gets its own functions. This is rule 4
  applied: "N identical -> base; different layout -> different function."

Do NOT refactor the code now. M3 is merged and the routine is single-use today;
lifting it into the shared leaf is the M4 target, not an M3 change.
