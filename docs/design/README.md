<!-- SPDX-License-Identifier: CECILL-C -->
# Design records -- index by status

The `design-*.md` documents are per-topic design records: the reasoning behind a decision, the
option space that was considered, and the evidence a claim rests on. There are 26 of them and they
accumulated in commit order with no index, which made the collection hard to use -- a reader could
not tell whether a document described the current system, a plan, or a road not taken.

**The files live in the parent directory (`../design-*.md`), not here.** Relocating them is a
separate call for the maintainer to make; this index exists so they can be found by status without
moving anything.

## The markers

Every `design-*.md` now carries one status marker in its header:

| Marker | Means | How to read the document |
|---|---|---|
| **LANDED** | The work shipped. | History plus rationale -- *not* a contract. For the current contract go to `../reference/`. Where a LANDED record still contains "pending" or "deferred" prose, its marker says what has since closed. |
| **ACTIVE** | Work in flight. | Live. Expect it to change under you. |
| **SUPERSEDED** | A later document or decision replaced it. | Read the successor first; kept only for the argument it lost. |
| **EXPLORATORY** | A spike. No commitment, usually no code. | Nothing here licenses a change. Useful for the option space and the constraints it found. |

Two things follow from this that are easy to get wrong:

- **A LANDED record is not the contract.** It says what was decided and why, frozen at decision
  time. The code-synced contract is `../reference/`, and where the two disagree the Reference
  wins (and the code wins over that).
- **Milestone numbers inside older documents may predate the renumbering.** The driver era took
  **M4**, SMP moved to **M5**, and the MMU horizon to **M6**; that decision is recorded in
  `../design-driver-era-scope.md` section 4. A document written before it may say "M4 = SMP".
  `../roadmap.md` is authoritative.

## LANDED

| Document | Subject |
|---|---|
| [`design-task9-mmio-driver.md`](../design-task9-mmio-driver.md) | The MMIO grant-at-spawn mechanism + the `arch_mpu_region_encodable` seam -- what makes an unprivileged userspace driver possible |
| [`design-mpu-commit-deferred.md`](../design-mpu-commit-deferred.md) | The enforcement-soundness seam: stash the region set at the switch decision, program it from the switch epilogue |
| [`design-cxx-under-mpu.md`](../design-cxx-under-mpu.md) | Full C++ (exceptions/STL/RTTI) from an unprivileged thread under enforcement, across four EH models |
| [`design-riscv-gp-split.md`](../design-riscv-gp-split.md) | Splitting the RISC-V `gp` small-data window kernel-vs-app, which is what let a U-mode throw work under PMP |
| [`design-m3-console-handover-stageii.md`](../design-m3-console-handover-stageii.md) | Handing the UART to a userspace driver, routing kernel output around it, and reclaiming it in a panic |
| [`design-m3-clock-select.md`](../design-m3-clock-select.md) | The clock-retune write side and its coherence tail (re-anchor, baud re-derive, timer re-arm) |
| [`design-spi-driver.md`](../design-spi-driver.md) | The XMC/USIC-SSC SPI driver -- the canonical per-thread PMSA MMIO-isolation proof |
| [`design-spi-driver-k64f-dspi.md`](../design-spi-driver-k64f-dspi.md) | The K64F DSPI driver behind KickCAT's ESC transport, designed within the coarse AIPS ceiling |
| [`design-spi-driver-stm32f411.md`](../design-spi-driver-stm32f411.md) | The F411 SPI1 loopback reference. Shipped but **never run on silicon** -- needs a bench swap |
| [`design-c6-driver.md`](../design-c6-driver.md) | The ESP32-C6 GPIO driver: per-thread PMP *plus* the coarse one-time APM/PMS open |
| [`design-rp2350.md`](../design-rp2350.md) | RP2350 Cortex-M33 bring-up; the IMAGE_DEF / vector-pin invariant is the part that still bites |
| [`design-rp2350-mpu-armv8m.md`](../design-rp2350-mpu-armv8m.md) | The ARMv8-M PMSAv8 MPU backend (`base`+`limit` + MAIR) behind the same seam |
| [`design-teensy-rt1062.md`](../design-teensy-rt1062.md) | Teensy 4.1 / i.MX RT1062 bring-up (first M7) |
| [`design-teensy-mpu-hang.md`](../design-teensy-mpu-hang.md) | Why an M7 stalled forever with no fault under enforcement, and the fixed-region wrap that fixed it |
| [`design-m4-fable-review.md`](../design-m4-fable-review.md) | The adversarial review of the M4 design principles, with the verification outcomes |

## ACTIVE

| Document | Subject |
|---|---|
| [`design-driver-era-scope.md`](../design-driver-era-scope.md) | The M4 gap list: what turns the M3 mechanisms into a fleet-wide capability. Section 4 records the milestone-ordering decision |
| [`design-m4-driver-matrix.md`](../design-m4-driver-matrix.md) | The per-board peripheral survey and the complexity-vs-gain backlog that bounds M4's scope |
| [`design-m4-driver-model.md`](../design-m4-driver-model.md) | How a driver is packaged: driver-lib class, service thread, or both (the ruling: both, service composed on the class) |
| [`design-kickcat-k64f.md`](../design-kickcat-k64f.md) | Running the KickCAT EtherCAT slave on KickOS. Sim stage landed; the K64F hardware path is still the plan |

## EXPLORATORY

| Document | Subject |
|---|---|
| [`design-multicore.md`](../design-multicore.md) | AMP-vs-SMP feasibility on RP2040 + RP2350 |
| [`design-multicore-ipc.md`](../design-multicore-ipc.md) | The cross-core IPC transport AMP needs and the kernel does not have |
| [`design-m5-smp.md`](../design-m5-smp.md) | SMP candidate ranking by the one gate that decides it, and the big-kernel-lock-first staged model |
| [`design-rp2350-hazard3.md`](../design-rp2350-hazard3.md) | Porting to the RP2350's RISC-V Hazard3 cores as a sibling of the M33 port |
| [`design-riscv-switch-cost.md`](../design-riscv-switch-cost.md) | Whether the RISC-V switch gap is worth a cooperative fast-path and/or Zcmp |
| [`design-armv8m-trustzone.md`](../design-armv8m-trustzone.md) | TrustZone as the strongest armv8-M realization of kernel confinement -- not an MPU replacement |
| [`design-mmu-era-exploration.md`](../design-mmu-era-exploration.md) | Growing from an MPU RTOS to real virtual address spaces (x86_64, i.MX8MP heterogeneous AMP) |

## SUPERSEDED

None currently. The marker exists because the category is real -- a design can be replaced
outright rather than shipped or abandoned -- and an empty section is itself informative: every
record here either landed, is in flight, or was always a spike.

## Not design records

Living in the same directory but not part of this collection:

- [`../m2-readiness.md`](../m2-readiness.md) -- the enforcement ledger: per-chip MPU fan-out and
  the M2/M3/M4.4 silicon proofs. The place to check "is this chip proven, and by what evidence".
- [`../m2-review-followups.md`](../m2-review-followups.md) -- follow-ups from the M2 review.
- [`../flashing.md`](../flashing.md) -- flash-tool backends and the non-J-Link paths.
- [`../reference/`](../reference/) -- the code-synced contract. [`../book/`](../book/) -- the
  durable how & why.
