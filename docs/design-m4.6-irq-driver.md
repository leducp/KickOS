<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M4.6 IRQ driver contract

> **Status: LANDED.** The full design gate, per-chip analysis and validation record are in
> the M4 IRQ driver record (archived `M4_IRQ_driver_record.md`). Its numbered sections remain there
> for citations to the original decisions. For the current notification ABI, use
> [IPC and notifications](reference/ipc-call-reply.md) and the code.

An IRQ line is a capability. `AUTH_IRQ` authorizes claiming a line; possession and rights on the
resulting capability authorize its use. The kernel masks a line in the interrupt handler and the
driver clears its device source before rearming. Claiming starts masked, and first wait arms the
line, so spawn delegation cannot leave an armed line without an owner.

The capability's final release detaches and masks the line. Driver death drops in-flight UART TX
with accounting and follows the console ownership/reclaim rules in
[the console reference](reference/console.md). Shared and grouped hardware lines are dispatched
as logical sources by the chip backend.

The buffered UART is userspace policy over that mechanism: an SPSC byte ring, a receive worker
and a service worker. Current notification delivery uses `Notification` objects and
`kos_notify_bind`, `kos_notify_wait`, `kos_notify` and `kos_irq_bind_notify`; the original
`kos_irq_*` delivery calls in the archived design were replaced by M8.13.
