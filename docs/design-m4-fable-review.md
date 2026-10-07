<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M4 driver design risks

> **Status: LANDED review.** The twelve original findings, verification notes and dated
> outcomes are in archived `M4_adversarial_review.md`.
> Finding numbers below remain stable for existing references.

## Settled findings

- **1:** The clock-retune path refuses a non-kernel-owned console before changing it.
- **4:** Call/reply priority donation landed; the current limits are in
  [`reference/ipc-call-reply.md`](reference/ipc-call-reply.md).
- **5:** A retained WAIT-bearing endpoint capability can hide a server death from
  clients. The handout right and no-receiver errno split are now part of the M10.1.4
  contract in [`design-m10-kernel-share.md`](design-m10-kernel-share.md).
- **12:** Service/thread pool and RAM costs must be admitted against the smallest
  board's configured budgets rather than inferred from a desktop-sized build.

## Remaining design constraints

- **6:** A standing clock-tree service needs an explicit two-phase rate-change
  protocol: quiesce affected drivers, bound a stalled participant, then publish the
  new rate. Init's one-time clock gating does not require that service.
- **8:** A shared-IRQ demultiplexer must not park on one subscriber while the shared
  line remains masked. Delivery needs per-subscriber nonblocking state and an
  explicit overflow policy.
- **10:** Large shared-buffer messages carry offsets or capabilities, not a raw
  pointer meaningful only in the sender's address space.

The remaining original findings concern GPIO grant geometry, clock authority,
peripheral minting and init's capability set. Consult the archived review when
revisiting those choices; the current implementation contracts are in `reference/`.
