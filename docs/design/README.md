<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design records -- index by status

The `design-*.md` documents are per-topic design records: the reasoning behind a decision, the
option space that was considered, and the evidence a claim rests on. Without an index a reader
cannot tell whether a document describes the current system, a plan, or a road not taken.

**The files live in the parent directory (`../design-*.md`), not here.** Relocating them is a
separate call for the maintainer to make; this index exists so they can be found by status without
moving anything.

**Coverage is total: 40 documents = 25 LANDED + 11 ACTIVE + 4 EXPLORATORY + 0 SUPERSEDED.** Every
`../design-*.md` appears in exactly one table, and no table names a file that does not exist.
`ls ../design-*.md | wc -l` is the check; run it before trusting the number.

The 2026-07-29 footprint capture the R2/R3/R4 rulings rest on is
[`archive/M4.5_footprint_meas.md`](../archive/M4.5_footprint_meas.md): a dated measurement record,
not a current footprint, and never in the re-grounding path.

## The markers

Every `design-*.md` carries a status line in its header. Most open with one of the four markers
below; the newest M5/M6 records state their status in prose on that line instead, and this index
files each of those under the marker that fits it:

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
- **Milestone numbers inside older documents may predate a renumbering.** The wave has been
  renumbered three times, and the LAST one SWAPPED TWO NUMBERS rather than shifting them. The driver
  era holds **M4** and **M5**; the **MMU is M6** and **multicore is M7** (they were the other way
  round until 2026-08-21); IPC and IRQ optimisation is **M8**, the driver era returns at **M9**, and
  KickCAT closes at **M10**. The first of those decisions is recorded in
  `../design-driver-era-scope.md` section 4 and the swap in `../../roadmap.md`, which is
  authoritative. So a document written before the swap may say "M6 = SMP" or "M7 = MMU" and mean the
  opposite of what it now reads as -- `../design-m7-smp.md` and `../design-mmu-era-exploration.md`
  were both in that state and have been RENAMED to the numbers they now carry, so a
  reference to either under its old `m6-` name is a stale link rather than a milestone claim.

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
| [`design-spi-driver-stm32f411.md`](../design-spi-driver-stm32f411.md) | The F411 SPI1 loopback reference. Shipped but **never run on silicon** -- the board is witnessed under PMSAv7, this app is not, and it wants the PA7->PA6 jumper |
| [`design-c6-driver.md`](../design-c6-driver.md) | The ESP32-C6 GPIO driver: per-thread PMP *plus* the coarse one-time APM/PMS open |
| [`design-rp2350.md`](../design-rp2350.md) | RP2350 Cortex-M33 bring-up; the IMAGE_DEF / vector-pin invariant is the part that still bites |
| [`design-rp2350-mpu-armv8m.md`](../design-rp2350-mpu-armv8m.md) | The ARMv8-M PMSAv8 MPU backend (`base`+`limit` + MAIR) behind the same seam |
| [`design-teensy-rt1062.md`](../design-teensy-rt1062.md) | Teensy 4.1 / i.MX RT1062 bring-up (first M7) |
| [`design-teensy-mpu-hang.md`](../design-teensy-mpu-hang.md) | Why an M7 stalled forever with no fault under enforcement, and the fixed-region wrap that fixed it |
| [`design-unprivileged-root.md`](../design-unprivileged-root.md) | Root starts unprivileged holding capabilities instead of starting privileged and demoting -- and the boards where that does not work. All five stages merged (`dde73ca`) |
| [`design-m4-fable-review.md`](../design-m4-fable-review.md) | The adversarial review of the M4 design principles, with the verification outcomes. Doubles as the driver era's **risk register**: each finding that events have tested carries an OUTCOME line (5 and 12 MATERIALISED as real defects; 4 is CLOSED; 6, 8 and 10 are OPEN -- M4.6.1 closed neither 6 nor 8, so they wait on the first clock-tree or shared-IRQ demux service) |
| [`design-flash-footprint.md`](../design-flash-footprint.md) | The footprint decision list: `-Os` rather than `-O1`/`-O2` (R2), the open 64-bit division helper (R3), the `.userheap` carve as policy rather than waste (R4), the `-Warray-bounds` pragma rather than `--param=min-pagesize=0`, and the standing LTO link defect. The numbers are a dated capture in [`archive/M4.5_footprint_meas.md`](../archive/M4.5_footprint_meas.md) |
| [`design-m4-driver-model.md`](../design-m4-driver-model.md) | How a driver is packaged: driver-lib class, service thread, or both (the ruling: both, service composed on the class) |
| [`design-m4.6-irq-driver.md`](../design-m4.6-irq-driver.md) | The M4.6.1 design gate: an unprivileged driver owning an interrupt line -- the proposed IRQ capability, handover at spawn, reclaim on driver death, shared/grouped lines, and the buffered userspace UART on top |
| [`design-capability-table.md`](../design-capability-table.md) | The capability table re-derived from a clean sheet: what a capability is here, why possession and not an access list, the size-class mix and the per-spawn interface deleted, the codec decoupled from provisioning, and one reservation law fleet-wide -- segmented storage taken whole at spawn, no growth -- across a range from 16 KiB to 8 GB |
| [`design-m4.8.2-host-unit-tests.md`](../design-m4.8.2-host-unit-tests.md) | The host unit-test layer: two seams, one at the syscall boundary and one at the arch boundary, and why the first needs no fixture. Section 7 item 7, the selftest-arm migration, is the residue |
| [`design-m4.7.9-fault-isolation.md`](../design-m4.7.9-fault-isolation.md) | Fault isolation: a thread dies, the system does not -- the fault-kill path landed in four commits |
| [`design-generic-driver-service.md`](../design-generic-driver-service.md) | One generic driver service, N chips: the descriptor ruling that M4.8.1 shipped |
| [`design-task-layer.md`](../design-task-layer.md) | A task as a set of threads, with the address space on Domain rather than Task |
| [`design-kill-and-slay.md`](../design-kill-and-slay.md) | The two-verb death ABI: kill stays cooperative, **slay** is forcible, and the victim runs its own teardown off a rebuilt context -- no reaper. Section 14 is what the design got WRONG; read it before section 3 |

## ACTIVE

| Document | Subject |
|---|---|
| [`design-driver-era-scope.md`](../design-driver-era-scope.md) | The M4 gap list: what turns the M3 mechanisms into a fleet-wide capability. Section 4 records the milestone-ordering decision |
| [`design-m4-driver-matrix.md`](../design-m4-driver-matrix.md) | The per-board peripheral survey and the complexity-vs-gain backlog that bounds M4's scope |
| [`design-kickcat-k64f.md`](../design-kickcat-k64f.md) | Running the KickCAT EtherCAT slave on KickOS. The K64F hardware path is still the plan; the tree links no KickCAT app, so the Stage A sim slave the body calls landed is not in `user/apps/` |
| [`design-style-enforcement.md`](../design-style-enforcement.md) | One mechanism enforcing house style across code, markdown and build files: the rule inventory bucketed by decidability, and why a formatter and a count gate both lose. Proposed, not built -- there is no `check_style.py` |
| [`design-m4.6.2-usb-cdc.md`](../design-m4.6.2-usb-cdc.md) | USB CDC console driver, the current M4.9.1 work. The number in the filename is the superseded one; `../../roadmap.md`'s ledger assigns M4.9.1 |
| [`design-m5-driver-set.md`](../design-m5-driver-set.md) | What "complete the driver set" owes, enumerated from the build system rather than from the plan: the per-chip capability matrix and the gaps it names. Header status: surveyed, scope not yet approved |
| [`design-m5-i2c-seam.md`](../design-m5-i2c-seam.md) | The I2C class contract, judged against three unrelated controllers and nine parts before an engine existed. The class header and the RX72M RIICa backend came out of it; the proxy and the service have not |
| [`design-m5-ipc-fastpath.md`](../design-m5-ipc-fastpath.md) | Bounding the IPC critical section: the measured call/reply baseline, which section 1 fixes as a measurement, and the fastpath judged against it |
| [`design-m5-kickcat-reality-check.md`](../design-m5-kickcat-reality-check.md) | KickCAT brought back at the end of the driver era to JUDGE the driver APIs rather than consume them: the SPI-class collision, the ruling, and what writing the backend found. Header status: written and compiled, never linked, never run |
| [`design-m7-state-inventory.md`](../design-m7-state-inventory.md) | Kernel state classified per-core versus genuinely global, and what the multi-instance sim corrected about that classification once part of it became executable. Read section 6 before the tables |
| [`design-m6-mmu.md`](../design-m6-mmu.md) | The M6 design contract: a unicore A53 on QEMU `virt`, with RV64 Sv39 as the litmus that falsifies the aspace seam and x86_64 falsifying the entry and boot paths, what it FREEZES (a high-half kernel, a domain becoming an address space so a task becomes a process, a 4 KiB granule, two backends before the seam is trusted), the seams below the arch boundary that are rewritten, and the step plan with the expected result of each step |

## EXPLORATORY

| Document | Subject |
|---|---|
| [`design-m7-smp.md`](../design-m7-smp.md) | SMP candidate ranking by the one gate that decides it, the big-kernel-lock-first staged model, the per-chip hardware mechanics and the cross-core IPC invariants |
| [`design-rp2350-hazard3.md`](../design-rp2350-hazard3.md) | Porting to the RP2350's RISC-V Hazard3 cores as a sibling of the M33 port |
| [`design-riscv-switch-cost.md`](../design-riscv-switch-cost.md) | Whether the RISC-V switch gap is worth a cooperative fast-path and/or Zcmp |
| [`design-mmu-era-exploration.md`](../design-mmu-era-exploration.md) | Growing from an MPU RTOS to real virtual address spaces. PARTLY ABSORBED: `design-m6-mmu.md` is the contract that came out of it and picked a different first target, so what stays live here is the platform exploration (x86_64 as a PC target, i.MX8MP heterogeneous AMP) |

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
