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
