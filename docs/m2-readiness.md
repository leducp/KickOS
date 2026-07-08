<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS pre-M2 readiness — task list

> The remaining work to put a **solid, cross-board-verified foundation** under M2
> (MPU enforcement — the design lives in `architecture.md` / `invariants.md`). M2
> wires `arch_mpu_apply` into the context-switch hook and adds per-task memory
> protection; it stacks directly on the scheduler / IRQ / timer paths, so those
> must be trustworthy on real silicon first. Ordered by priority.

## 1. Thread-slot reclamation (FIRST PRIORITY) — ✅ DONE

`thread_spawn` reclaims EXITED pool slots at spawn (twin of the sem freelist),
generation-tagged handles, full TCB/context reset on reuse. Validated: sim
(deterministic) + qemu-arm (PendSV) + qemu-riscv (msip) stress churn all PASS; the
full selftest (15/15 on sim) now runs on a **2-slot pool** (`-DKICKOS_MAX_THREADS=2`),
which unblocks the armv6m QEMU (`microbit`) selftest. Passed the 10-angle review gate.
(The qemu-riscv stress *soak* flaky-crashes QEMU under heavy PMP-reprogram load — guest
is clean, not in CI; keep churn totals modest.)

## 2. Maximum hardware-board coverage — the final on-silicon pass

**This is the single batched HW-validation pass** (per the HW-test deferral policy:
batch per-board silicon re-validation until after telemetry lands, item 3). Run the
full suite (`-DKICKOS_ENABLE_SELFTEST=ON`: selftest + stress + `sched_exit`) on every
board we can physically flash, capture over each board's console, record pass/fail.

Run the full suite (`-DKICKOS_ENABLE_SELFTEST=ON`: selftest + stress + `sched_exit`)
on every board we can physically flash, and record pass/fail. Batch per the HW-test
deferral policy; capture over each board's console.

| Board | Arch | Status to reach |
|-------|------|-----------------|
| frdmk64f | armv7m | ✅ selftest 14/14 + stress soak (120k handoffs) on silicon |
| rx72m | rxv3 | ✅ runs on silicon; tickless re-arm bug fixed generically; bench 96cyc/400ns recorded. Re-run selftest+stress to close. |
| xmc4800-relax | armv7m | **blocked by the `multi_wait` deadlock (item 5)**; then selftest + stress |
| f411disco / f302nucleo | armv7m/M4 | selftest + stress |
| picopi (RP2040) | armv6m | `sched_exit`; full selftest now possible (item 1 unblocked small pools) |
| esp32-wroom / esp32c6-wroom | lx6 / rv32imac | bring up flashing (currently build-only), then suite |
| microbit (nrf51) | armv6m | **not a physical target** — a QEMU armv6m vehicle; covered by CI (`-M microbit`) |
| qemu / qemu-riscv | armv7m / rv32imac | ✅ automated GitHub CI (item 6); the QEMU functional gate, not silicon |

## 3. Telemetry capture per board

With the observability subsystem (`docs/telemetry.md`, `KICKOS_TELEMETRY=rtt`,
`KICKOS_CONSOLE=rtt|both`), record context-switch / syscall / IRQ traces on each
silicon board and archive the baselines. Confirms the trace clock + RTT wire format
hold on real hardware ahead of M2, and gives a pre-MPU performance reference to
measure the M2 `arch_mpu_apply` switch-hook cost against.

## 4. Context-switch + IRQ-entry bench baselines

Fill the per-board bench table (bench app, `KICKOS_BENCH=ON`). Have: RX72M 96 cyc /
400 ns, K64F 77 cyc / 641 ns. Record the rest; these are the M2 regression baseline.

## 5. Open bugs to resolve or track

- **XMC `multi_wait` deadlock** — XMC-chip-specific (K64F passes, so not armv7m-general).
  Spawned workers never run while main is in a tickless timer-sleep. Needs a no-reset
  J-Link attach or SWO trace (J-Link resets XMC on connect, defeating post-mortem).
  **Gates the XMC half of the M2 reference pair — highest-leverage open item.**
- ~~RX bench HW re-validation~~ ✅ resolved: the tickless re-arm was a real one-shot-reset
  bug, fixed generically; RX runs on silicon and the 96cyc/400ns baseline is recorded.
- ~~microbit full selftest~~ ✅ resolved: it exposed a real selftest leak — raw-handle
  tests never destroyed their semaphores, so the suite overran microbit's 4-slot pool.
  Fixed the suite (every test reclaims its semaphore; peak concurrent = 4), NOT the pool.
  armv6m now runs the full selftest in CI. (The refusal to defer caught the bug — see the
  no-deferral principle.)

## 6. GitHub Actions CI — ✅ DONE

`.github/workflows/ci.yml` runs the green gates on every push/PR (was hand-run).
Scoped to deterministic, host-runnable gates — no flaky or HW-only steps:
- **Build matrix**: configure + build every board whose toolchain is apt-installable
  — the ARM families + RISC-V — catching build/link/linker-script regressions. Plus
  the `oot_export` in-tree/out-of-tree gate (runs in the sim ctest).
- **sim** (`-DKICKOS_ENABLE_SELFTEST=ON`): full `ctest` — selftest, stress (incl. the
  thread-reclaim churn), `sched_exit`, `mpu_fault`, telemetry golden-vector. This is
  the authoritative deterministic gate.
- **qemu-arm** (`mps2-an386` + `microbit`): `ctest` — the **full `selftest` TAP suite**
  (same binary as sim, on real armv7m PendSV) + hello + sched_exit + fp_switch. These
  clock off semihosting `SYS_CLOCK`, which QEMU ≤ 10 (ubuntu-latest ships 8.2) freezes
  while the core halts in WFI — a timed sleep with every thread idle never wakes. Fixed
  in the kernel: `arch_idle_wait` spins (not WFI) on the QEMU-only mps2/nrf51 chips.
  (icount does NOT fix it — verified.) `rr_interleave` scales its RR quantum to the
  target's clock granule, so it runs even on the coarse clock (no skip). **microbit
  (armv6m)** runs the full suite too — its small pool (`MAX_SEMAPHORES=4`) forced the
  suite to be pool-honest (each test reclaims its semaphores), so armv6m is gated, not
  a silent hole.
- **qemu-riscv** (`virt`): `ctest` — the **full `selftest` suite** (on rv32imac msip) +
  hello + sched_exit; never the stress soak (QEMU host-crashes flakily under heavy
  PMP-reprogram load; guest is clean — see the qemu-riscv-crash note). Uses the **xPack
  `riscv-none-elf` toolchain** (pinned), symlinked to the `riscv64-unknown-elf-` prefix:
  apt's riscv gcc ships no rv32imac newlib/libgloss and Ubuntu has no drop-in lib
  package like ARM's. Also build-checks `esp32c6-wroom` + `qemu-riscv-bench`.
  (mtime clock → no WFI idle issue.)
- Toolchains: ARM = apt `gcc-arm-none-eabi` **+ `libnewlib-arm-none-eabi`** (the C
  library the split apt package omits); RISC-V = pinned xPack tarball.

RX and Xtensa use vendor toolchains (not in apt) — kept out of CI for now (one step
at a time); build those boards manually. Silicon likewise stays a manual/self-hosted
step per the HW-test deferral policy.

## Then: M2 proper

Per `architecture.md`: MPU-enforcement fan-out — reference pair (RISC-V PMP/NAPOT +
XMC v7-M PMSA) → K64F SYSMPU → RX → tail; plus the arch-independent security model
(domains, per-thread private stacks, syscall-arg/user-pointer validation, pow2 region
placement). Capabilities + authenticated grants are M3.
