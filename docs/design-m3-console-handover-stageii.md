<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M3 console handover design

> **Status: LANDED.** The code-synced routing and publisher contract is in
> [`reference/console.md`](reference/console.md); the `console-*` invariants are in
> [`reference/invariants.md`](reference/invariants.md).

The kernel console has an ownership state separate from its buffered-versus-polled
choice. `KERNEL_OWNED` permits kernel UART output. `HANDING_OFF` drains chip writers.
`USER_OWNED` makes the kernel leave the device untouched while RTT can still carry
output. `RECLAIMED` routes output through the polled panic-safe path. The ownership
check precedes every kernel chip write.

## Handover

`kos_console_publish` requires `AUTH_CONSOLE`. Under one `IrqLock`,
`console_tx_deinit` flushes to shift idle, disables the TX interrupt, detaches and masks
the line, and disarms the ring. Publish retains a kernel reference on the endpoint,
updates the stdout target, drains writers that sampled the old state, then completes
the move to `USER_OWNED`. The drain yields so a lower-priority writer can finish.
A publisher cannot start console-dependent tasks between publish and a successful
driver spawn. It drops its own WAIT-bearing capability after spawning the driver;
otherwise the driver's death cannot take `recv_holders` to zero and wake senders.

Child threads receive a send-only stdout capability in reserved slot 0 after publish.
An earlier thread keeps its original slot. The publish-aware writers (`_write`,
`kickos::emit` and TAP output) inspect each caller's slot for each write and fall back
for only the unsent remainder after a failure. A new publish uses a fresh endpoint;
clients already holding the old one are not silently retargeted.

The userspace driver holds the UART window and the endpoint's WAIT right. It must not
use libc stdio, which would send to its own endpoint and block. Its priority must let
it run while clients wait; endpoint rendezvous does not supply priority inheritance.

## Reclaim and remaining boundary

A panic moves the console to `RECLAIMED` before calling the chip's idempotent
`arch_console_reclaim` body, then prints through the polled path. Driver death records
a reclaim request when the last receiver goes away; reclaim waits until no live task
holds the console window, so it cannot reprogram the UART under a surviving IRQ thread.
Boards without a reclaim body keep the no-op fallback. The precise routing and
publisher order are in the reference contract above.

### Ruling 7: multicore reclaim

On a shared kernel, another core may still have the driver running when one core
panics. Reclaim must fence that device owner before the polled panic writer takes the
UART. The single-core masked-interrupt argument does not provide that fence.
