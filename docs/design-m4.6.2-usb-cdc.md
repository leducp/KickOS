<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M4.9.1 USB CDC console

> **Status: LANDED.** This was first numbered M4.6.2. The controller comparison, errata,
> staging and validation analysis are in [the original design record](archive/M4_USB_CDC_design_record.md).
> Use [the console reference](reference/console.md) and code for current handover behavior.

The USB CDC console uses the IRQ capability and two-thread driver pattern from M4.6. RP2040
and RP2350 share one USB device backend with chip-specific startup values. The i.MX RT1062
uses a separate EHCI-derived backend. The driver receives only its controller MMIO window;
clocks and PHY setup stay in privileged bring-up.

On driver death, the board's independent kernel console is the fallback. Panic output must use
a bounded polling path rather than rely on the stopped driver. RP USB requires a valid 48 MHz
clock and must decline bring-up on the crystal-failure path. Low-power changes must keep the USB
clock alive while the console is active.

Descriptor and setup parsing can be tested without a board. Enumeration, panic flushing and
controller behavior need a real USB host and silicon; the original record distinguishes those
witnesses from unverified assumptions.
