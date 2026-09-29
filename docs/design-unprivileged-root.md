<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Unprivileged root

> **Status: LANDED.** The five-stage implementation record, board limits and verification
> arguments are in [the original design](archive/M4_unprivileged_root_record.md).
> Current rules are in [invariants](reference/invariants.md),
> [porting](reference/porting.md) and [boards](reference/boards.md).

Root starts with the unprivileged thread posture and explicit authority for bring-up. Its
first frame carries that posture; there is no later demotion transition or region-set
recomposition. The idle thread retains the kernel posture. A board's ability to enforce the
privilege distinction is classified per board, because ARMv6-M includes cores both with and
without that extension; LX6 and the sim have their own stated limits.

Privileged register writes needed during bring-up pass through narrow kernel seams. MMIO
access, clock setup and later revocation remain constrained by the relevant capability and
board contracts. The archive retains the rejected demotion design and the per-board witness
ledger.
