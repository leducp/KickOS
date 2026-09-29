<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M4 driver coverage: scope decision

> **Status: LANDED.** The dated per-board survey, scores and original backlog are in
> [`archive/M4_driver_coverage_survey.md`](archive/M4_driver_coverage_survey.md).
> Current work is assigned by `roadmap.md` and `TODO.md`.

M4 sought broad peripheral coverage on four neutrality boards: ESP32-C6, XMC4800,
K64F and RX72M. Coverage was an aspiration rather than a promise to implement every
peripheral in the milestone. Candidates were ranked by API-discovery value, practical
use, cross-vendor coverage and bring-up cost. The ESP32-C6 and RX72M receive an
additional cost for print-only diagnosis without a probe.

The strongest shared API tests were console/UART on all four boards; four unrelated SPI
controllers; addressed I2C transactions; analog input through ADC and die-temperature
sensors; and PWM across different timer architectures. A console and one bus driver
per board formed the minimum neutrality proof. Analog input and PWM were the next
high-value API-discovery candidates. DMA, network controllers and CAN need separate
resource and traffic models; a single device grant is insufficient for shared DMA.

The archived survey records what each board exposed and what the tree drove at the
time of the M4 scope decision. Its coverage labels and priority numbers are historical,
not a live inventory.
