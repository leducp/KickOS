<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design documents by status

The `../design-*.md` files state shipped designs, active work and exploratory proposals.
Dated measurements, step logs and superseded reasoning are archived in git (`../README.md`), where
a short design page cites them. For code-synced behavior, use `../reference/` and the code.

**Coverage is total: 51 documents = 31 LANDED + 11 ACTIVE + 9 EXPLORATORY + 0 SUPERSEDED.** Every
`../design-*.md` appears in exactly one table, and no table names a file that does not exist.
`ls ../design-*.md | wc -l` is the check; run it before trusting the number.

## The markers

Every `design-*.md` carries a status line. This index groups them as follows:

| Marker | Means | How to read the document |
|---|---|---|
| **LANDED** | The work shipped. | The design explains its rules; `../reference/` and code settle current behavior. Dated implementation records are archived. |
| **ACTIVE** | Work in flight. | Live. Expect it to change under you. |
| **SUPERSEDED** | A later document or decision replaced it. | Read the successor first; kept only for the argument it lost. |
| **EXPLORATORY** | A spike. No commitment, usually no code. | Nothing here licenses a change. Useful for the option space and the constraints it found. |

`../../roadmap.md` owns milestone numbering. M4 and M5 are the driver era, M6 the MMU, M7
multicore, M8 IPC/IRQ optimization and M9 kernel concurrency. Earlier drafts used
different numbers; the current filenames use this order.

## LANDED

| Document | Subject |
|---|---|
| [`design-m9-lock-bound.md`](../design-m9-lock-bound.md) | M9.1's ticket-lock wait model and backend limits; derivation and measurements archived |
| [`design-m9.4-rings.md`](../design-m9.4-rings.md) | Per-pair scheduler rings under one lock; the proposed unlocked switch stage was refused |
| [`design-m9.5-bkl-options.md`](../design-m9.5-bkl-options.md) | M9.5's decision: one big kernel lock for every shared-kernel transaction, CLH arbitration on x86_64 and ticket arbitration elsewhere, with the x86, ARM64 and LX6 measurements behind it |
| [`design-task9-mmio-driver.md`](../design-task9-mmio-driver.md) | The MMIO grant-at-spawn mechanism + the `arch_mpu_region_encodable` seam -- what makes an unprivileged userspace driver possible |
| [`design-mpu-commit-deferred.md`](../design-mpu-commit-deferred.md) | The enforcement-soundness seam: stash the region set at the switch decision, program it from the switch epilogue |
| [`design-cxx-under-mpu.md`](../design-cxx-under-mpu.md) | Full C++ (exceptions/STL/RTTI) from an unprivileged thread under enforcement, across four EH models |
| [`design-riscv-gp-split.md`](../design-riscv-gp-split.md) | Splitting the RISC-V `gp` small-data window kernel-vs-app, which is what let a U-mode throw work under PMP |
| [`design-m3-console-handover-stageii.md`](../design-m3-console-handover-stageii.md) | Handing the UART to a userspace driver, routing kernel output around it, and reclaiming it in a panic |
| [`design-m3-clock-select.md`](../design-m3-clock-select.md) | The clock-retune write side and its coherence tail (re-anchor, baud re-derive, timer re-arm) |
| [`design-spi-driver.md`](../design-spi-driver.md) | The XMC/USIC-SSC SPI driver -- the canonical per-thread PMSA MMIO-isolation proof |
| [`design-spi-driver-k64f-dspi.md`](../design-spi-driver-k64f-dspi.md) | The K64F DSPI driver behind KickCAT's ESC transport, designed within the coarse AIPS ceiling |
| [`design-spi-driver-stm32f411.md`](../design-spi-driver-stm32f411.md) | The F411 SPI1 loopback reference. Shipped but **never run on silicon** -- the board is witnessed under PMSAv7, this app is not, and it wants the PA7->PA6 jumper |
| [`design-c6-driver.md`](../design-c6-driver.md) | The ESP32-C6 GPIO driver: per-thread PMP *plus* the coarse one-time APM/PMS open |
| [`design-rp2350.md`](../design-rp2350.md) | RP2350 Cortex-M33 bring-up; the IMAGE_DEF / vector-pin invariant is the part that still bites |
| [`design-rp2350-mpu-armv8m.md`](../design-rp2350-mpu-armv8m.md) | The ARMv8-M PMSAv8 MPU backend (`base`+`limit` + MAIR) behind the same seam |
| [`design-teensy-rt1062.md`](../design-teensy-rt1062.md) | Teensy 4.1 / i.MX RT1062 bring-up (first M7) |
| [`design-teensy-mpu-hang.md`](../design-teensy-mpu-hang.md) | Why an M7 stalled forever with no fault under enforcement, and the fixed-region wrap that fixed it |
| [`design-unprivileged-root.md`](../design-unprivileged-root.md) | Root's unprivileged start and board limits; the five-stage record is archived |
| [`design-m4-fable-review.md`](../design-m4-fable-review.md) | The live M4 risk constraints, with numbered findings and a link to the archived adversarial review |
| [`design-flash-footprint.md`](../design-flash-footprint.md) | The footprint decision list: `-Os` rather than `-O1`/`-O2` (R2), the open 64-bit division helper (R3), the `.userheap` carve as policy rather than waste (R4), the `-Warray-bounds` pragma rather than `--param=min-pagesize=0`, and the standing LTO link defect. The numbers are a dated capture in archived `M4.5_footprint_meas.md` |
| [`design-m4-driver-model.md`](../design-m4-driver-model.md) | How a driver is packaged: driver-lib class, service thread, or both (the ruling: both, service composed on the class) |
| [`design-m4.6-irq-driver.md`](../design-m4.6-irq-driver.md) | IRQ capability and userspace UART rules; the original ABI and per-chip analysis are archived |
| [`design-capability-table.md`](../design-capability-table.md) | Task-relative handles, possession rights and segmented reservation; the derivation is archived |
| [`design-m4.8.2-host-unit-tests.md`](../design-m4.8.2-host-unit-tests.md) | Host U and K seams; selftest-arm migration remains open |
| [`design-m4.7.9-fault-isolation.md`](../design-m4.7.9-fault-isolation.md) | Fault-kill conditions, exit route and reporting limits |
| [`design-generic-driver-service.md`](../design-generic-driver-service.md) | Descriptor-based driver bring-up over class-specific services |
| [`design-task-layer.md`](../design-task-layer.md) | Task membership and lifetime, distinct from the Domain address space |
| [`design-kill-and-slay.md`](../design-kill-and-slay.md) | Cooperative kill and forcible slay; original mechanism and landing corrections archived |
| [`design-m6-mmu.md`](../design-m6-mmu.md) | M6 address-space contract; completed steps and the three-backend seam derivation are archived |
| [`design-m4-driver-matrix.md`](../design-m4-driver-matrix.md) | The M4 driver-coverage scope decision and a link to the dated per-board survey |
| [`design-m4.6.2-usb-cdc.md`](../design-m4.6.2-usb-cdc.md) | USB CDC console constraints; originally numbered M4.6.2, shipped as M4.9.1 |

## ACTIVE

| Document | Subject |
|---|---|
| [`design-driver-era-scope.md`](../design-driver-era-scope.md) | The M4 gap list: what turns the M3 mechanisms into a fleet-wide capability. Section 4 records the milestone-ordering decision |
| [`design-kickcat-k64f.md`](../design-kickcat-k64f.md) | Running the KickCAT EtherCAT slave on KickOS. The K64F hardware path is still the plan; the tree links no KickCAT app, so the Stage A sim slave the body calls landed is not in `user/apps/` |
| [`design-m5-driver-set.md`](../design-m5-driver-set.md) | What "complete the driver set" owes, enumerated from the build system rather than from the plan: the per-chip capability matrix and the gaps it names. Header status: surveyed, scope not yet approved |
| [`design-m5-i2c-seam.md`](../design-m5-i2c-seam.md) | The I2C class contract, judged against three unrelated controllers and nine parts before an engine existed. The class header and the RX72M RIICa backend came out of it; the proxy and the service have not |
| [`design-m5-ipc-fastpath.md`](../design-m5-ipc-fastpath.md) | Constraints on a proposed IPC fastpath; the measured baseline, corrected phase data and candidate analysis are archived |
| [`design-m5-kickcat-reality-check.md`](../design-m5-kickcat-reality-check.md) | KickCAT brought back at the end of the driver era to JUDGE the driver APIs rather than consume them: the SPI-class collision, the ruling, and what writing the backend found. Header status: written and compiled, never linked, never run |
| [`design-m7-state-inventory.md`](../design-m7-state-inventory.md) | Kernel state classified per-core versus genuinely global, and what the multi-instance sim corrected about that classification once part of it became executable. Read section 6 before the tables |
| [`design-multicore.md`](../design-multicore.md) | Shared-kernel and AMP predicate, rules, thread placement and open boundaries; completed stage record archived |
| [`design-m10-composition.md`](../design-m10-composition.md) | M10's static composition contract: the chip, board and composition files, admission rules, emitted table, lookups and init lifecycle |
| [`design-m10-kernel-share.md`](../design-m10-kernel-share.md) | M10.1's kernel contracts: x86_64 address spaces on q35, six ABI changes and their header touchpoints |
| [`design-m10-toolchain.md`](../design-m10-toolchain.md) | M10.2's toolchain: the pinned sources, the six families and their multilibs, the Conan recipe and what it deletes |
| [`design-m10-target.md`](../design-m10-target.md) | M10.4's target side: the init's walk against the kernel calls, packaged drivers under it, the cost model's corrections, the lookups, the system targets and their link-time asserts, the golden runs |
| [`design-m10-fleet.md`](../design-m10-fleet.md) | M10.5's fleet: chip files with the kernel's chip headers generated from them, every board's default, every app on compositions, lines from the composition, the partition build, x86_64 through `add_executable` and the deletions |
| [`design-m9-entry-envelope.md`](../design-m9-entry-envelope.md) | What the M8.12 entry metrics can support for M9; the full recomputation and capture audit are archived |

## EXPLORATORY

| Document | Subject |
|---|---|
| [`design-m7-smp.md`](../design-m7-smp.md) | Historical SMP candidate spike; the hardware analysis and staging are archived |
| [`design-rp2350-hazard3.md`](../design-rp2350-hazard3.md) | Porting to the RP2350's RISC-V Hazard3 cores as a sibling of the M33 port |
| [`design-riscv-switch-cost.md`](../design-riscv-switch-cost.md) | Whether the RISC-V switch gap is worth a cooperative fast-path and/or Zcmp. **ANSWERED AND REFUSED (2026-09-18)**: neither lever is built, the page carries the numbers and the four tests that would reopen it. It stays here because nothing was committed to code, not because the question is open |
| [`design-m9.5-local-ipc.md`](../design-m9.5-local-ipc.md) | An owner-local IPC path on x86 beside the lock, measured on pinned KVM. **REMOVED**: the mixed workload did not pay for a second exclusion protocol, and the kernel keeps the single lock path |
| [`design-m9.5-ipc-lock-feasibility.md`](../design-m9.5-ipc-lock-feasibility.md) | A source-level gate on locking whole IPC transactions per object: no small prototype is ready to benchmark, because a safe one needs a new lifetime and publication protocol across IPC, capabilities, deadlines, the scheduler and the switch |
| [`design-mmu-era-exploration.md`](../design-mmu-era-exploration.md) | Growing from an MPU RTOS to real virtual address spaces. PARTLY ABSORBED: `design-m6-mmu.md` is the contract that came out of it and picked a different first target, so what stays live here is the platform exploration (x86_64 as a PC target, i.MX8MP heterogeneous AMP) |
| [`design-style-enforcement.md`](../design-style-enforcement.md) | One mechanism enforcing house style across code, markdown and build files: the rule inventory bucketed by decidability, and why a formatter and a count gate both lose. Proposed, not built -- there is no `check_style.py` |
| [`design-m9-reference-kernels.md`](../design-m9-reference-kernels.md) | What the M9.0 kernel survey can and cannot justify for KickOS; the nineteen dated source rows are archived |
| [`design-stack-safety-research.md`](../design-stack-safety-research.md) | M9.0 kept per-thread kernel continuations; research and audit evidence archived |

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
