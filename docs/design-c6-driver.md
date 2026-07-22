<!-- SPDX-License-Identifier: CECILL-C -->
# Design brief: ESP32-C6 unprivileged GPIO driver -- the canonical PMP + APM per-thread peripheral-isolation reference

IMPLEMENTED + PROVEN on silicon (2026-07-17): `user/apps/c6blink` blinks GPIO10 through
the granted 8 B PMP window (APM opened) then an ungranted `GPIO_ENABLE` poke PMP-faults
(mcause=7). The earlier "boot-loop" was an elf2image RAM-only-header flag error, not a
code bug. The RISC-V analog of the F411 canonical PMSA proof
(`design-spi-driver-stm32f411.md`): prove a granted per-thread peripheral window WORKS
and an ungranted poke FAULTS, per thread -- but on the C6 that per-thread PMP line sits
on top of a coarse, one-time **APM** background permit the fleet has never driven. Builds
on the landed MMIO-grant seam (`design-task9-mmio-driver.md`;
`kos_thread_params.mmio_base/mmio_size`), the k64drv privileged-shim -> unprivileged-driver
structure (`user/apps/k64drv/main.cc`), and the rv32imac PMP backend + U-mode fault routing
(`arch/riscv/rv32imac/arch_rv32imac.cc`).

## Why this exists -- the two-gate model the C6 forces (read first)
On the C6, an HP-CPU access to an HP peripheral in **user (REE) mode** passes **two**
permission units in series (TRM 16.1, Table 16.1-1):

1. **PMP** -- CPU-side, per-hart, checked FIRST. The KickOS per-thread MMIO grant programs
   it (`kickos_arch_mpu_commit`, NAPOT; `arch_mpu_apply` stashes the grant on switch-in). This
   is the **per-thread** line: a granted window works, an
   ungranted access with no matching entry FAULTS (RISC-V PMP is fail-closed).
2. **APM** (Access Permission Management, TRM Ch.16) -- bus-side, per **security mode**
   (TEE/REE0/REE1/REE2), NOT per-thread. Checked only if PMP passes ("If the PMP check
   fails, the APM permission management will not be triggered", 16.1). Default posture:
   the HP CPU is TEE (=machine mode); **any drop to user mode = REE, and APM by default
   blocks REE0/REE1/REE2 access to every peripheral** (16.3.2 Note). So without a one-time
   APM open, a U-mode driver reaches nothing -- even with a correct PMP grant.

This is structurally like the K64F AIPS-open (a coarse bus-side gate the shim must open),
but **inverted in strength**: on K64F the AIPS bridge is the ONLY peripheral gate and it is
coarse/per-slot/all-user, so there is no per-thread peripheral line at all (k64drv proved
the SYSMPU window is inert). On the C6, APM is the coarse background permit and **PMP still
draws a genuine per-thread line on top of it** -- so the C6 achieves what K64F structurally
cannot. This driver first-proves that two-gate model on C6 silicon.

## Peripheral chosen: a plain GPIO toggle (blink) on a free header pin -- and why
Simplest peripheral that exercises the model with the fewest moving parts: **no IRQ**, a
single tiny MMIO window, an externally observable output (LED/scope). It isolates exactly
the one thing being proven -- PMP per-thread window + APM background open -- with no IRQ
plumbing confounding the result.

**Not the onboard WS2812 (LED2, GPIO8):** RMT-driven, strapping pin, already a privileged
kernel diag (`arch_diag_led_*`). Use a plain non-strapping header GPIO driven push-pull.

**Not a timer-IRQ driver (the k64drv PIT analog) -- for a first cut.** The timer path is
the natural follow-on but is NOT a pure userspace addition on the C6 today, unlike on
ARM/K64F. The tier-1 userspace IRQ path (`kos_irq_register/wait/ack`) reaches
`kickos_isr_irq(line)` on the C6 only via the **software-inject doorbell** (INTPRI FROM_CPU_0
-> CPU int 31 -> `switch.S .Lext` -> `g_inject_line`) -- test scaffolding, not a real device.
The one **real** device wired through the interrupt matrix -- UART0 TX -> CPU int 30 ->
`.Lextdev` -> `kickos_rv_ext_dispatch_dev` -- is HARD-CODED to the console line
(`chip_esp32c6.cc`). Adding a real timer source therefore requires generalizing the C6
device-IRQ dispatch (PLIC-claim demux to a logical line, or a second device CPU int +
vector) -- an arch/chip change touching `chip_esp32c6.cc` (+ possibly `switch.S`), which
breaks the "pure addition, no kernel change" property that made k64drv/f411spi clean. GPIO
first proves the isolation model; the timer-IRQ driver lands once the device-IRQ fan-out is
generalized (see Open questions).

## Confirmed hardware facts (ESP32-C6 TRM v1.2; cited)
| Fact | Value | TRM citation |
|---|---|---|
| GPIO Matrix base | `0x6009_1000` (block `..0x6009_1FFF`) | mem map Table 5.3-2 |
| `GPIO_OUT_REG` | off `0x0004` (R/W) | 7.15.1 GPIO Matrix Register Summary |
| `GPIO_OUT_W1TS_REG` | off `0x0008` (WT, set-1s) | 7.15.1 |
| `GPIO_OUT_W1TC_REG` | off `0x000C` (WT, clear-1s) | 7.15.1 |
| `GPIO_ENABLE_REG` / `_W1TS` / `_W1TC` | off `0x0020` / `0x0024` / `0x0028` | 7.15.1 |
| `GPIO_IN_REG` (readback) | off `0x003C` (RO) | 7.15.1 |
| `GPIO_FUNCn_OUT_SEL_CFG_REG` | off `0x0554 + 0x4*n` | 7.15.1 |
| IO MUX base (escalation) | `0x6009_0000` | mem map Table 5.3-2 |
| PCR clock/reset base (escalation) | `0x6009_6000` | mem map Table 5.3-2 |
| HP peripheral aperture (HP_PERI) | `0x6000_0000 .. 0x600A_FFFF` | Table 16.1-1 note 2 |
| HP CPU over HP_PERI | **PMP + APM** (PMP first) | Table 16.1-1 + 16.1 |
| TEE=M-mode always R/W/X; REE=U-mode | -- | 16.3.1 |
| HP TEE base / `TEE_M0_MODE_CTRL_REG` | `0x6009_8000` / off `0x0000+0x4*n`, reset 0 (=> U-mode is REE0) | mem map + Reg 16.53 |
| HP_APM base | `0x6009_9000` | mem map Table 5.3-2 |
| `HP_APM_REGION_FILTER_EN_REG` | off `0x0000`, reset `0x01` (region 0 on) | Reg 16.1 |
| `HP_APM_REGIONn_ADDR_START` | off `0x0004+0xC*n`, reset 0 | Reg 16.2 |
| `HP_APM_REGIONn_ADDR_END` | off `0x0008+0xC*n`, reset `0xFFFFFFFF` | Reg 16.3 |
| `HP_APM_REGIONn_ATTR_REG` | off `0x000C+0xC*n`, reset 0; bits R0_X=0,R0_W=1,R0_R=2,R1_X=3..R2_R=8 | Reg 16.4 |
| `HP_APM_FUNC_CTRL_REG` | off `0x00C4`, reset `0xF` (M0-M3 enforcing) | Reg 16.5 |
| Default APM posture | HP CPU TEE, others REE2; **APM blocks all REE0/1/2** | 16.3.2 Note |
| APM regs writable only in TEE mode | -- | 16.3.2 Note |
| Illegal APM access | **read returns 0, write dropped, + APM interrupt** (NOT a load/store trap) | 16.5 |

**Confirmed on silicon (2026-07-17, c6blink runs -- APM open admits the window, ungranted poke PMP-faults; so the M0 data path, TEE_M0_MODE=REE0, and GPIO10-free all hold). Originally flagged for bench:** (a) the free header pin -- recommend a
non-strapping, non-USB/JTAG/console GPIO (**GPIO10** as a first pick; GPIO8/9/15 are
strapping, 12/13 USB-JTAG, 16/17 the CH343P console UART) -- confirm against
`ESP32-C6-DEV-KIT-N8-Schematic.pdf` that the chosen pin is broken out and free. (b) That the
HP-CPU HP-peripheral APM data path is **M0** -- the 16.5 illegal-access worked example and
the default posture both use M0, and 16.3.2 states M1 carries "all masters except HP CPU
and LP CPU", so HP CPU = M0 by elimination; confirm on silicon via `HP_APM_M0_*` exception
registers. (c) The exact `TEE_M0_MODE` encoding for REE0 -- the extracted text is garbled;
reset 0 maps U-mode to REE0, so no write is strictly needed, but confirm.

## Granted window: 8 B NAPOT over W1TS + W1TC (the per-thread capability)
**`mmio_base = 0x6009_1008`, `mmio_size = 8`.** Covers exactly `GPIO_OUT_W1TS` (`0x08`) and
`GPIO_OUT_W1TC` (`0x0C`) -- the two write-only atomic set/clear registers the driver needs.
`arch_mpu_region_encodable(0x60091008, 8)` (`arch_rv32imac.cc`): size 8 >= min 8, pow2, and
`0x60091008 & 7 == 0` -> naturally aligned -> **encodable** as one PMP NAPOT entry. Attr is
`R|W`, **X cleared** (PMP has no device-memory type -- `ARCH_MPU_DEV` is a documented no-op
on PMP; MMIO is plain RW-NX).

Why W1TS/W1TC and not `GPIO_OUT`: atomic set/clear touch only the driver's pin bit with no
read-modify-write of the shared output latch. `GPIO_ENABLE` (`0x20`), `GPIO_IN` (`0x3C`),
and `GPIO_FUNCn_OUT_SEL` (`0x554+`) all fall OUTSIDE the 8 B window -- so the driver can
drive its pin but cannot change pin direction, cannot re-mux, and cannot read peripheral
input registers. Those stay privileged (the escalation surfaces). Fallback if `GPIO_OUT`
RMW is ever wanted: a 16 B NAPOT at `0x6009_1000` (`0x00..0x0F`) -- still excludes
`ENABLE`/`IN`/`FUNC_OUT_SEL`.

**Honest granularity caveat (a register limit, not a PMP limit):** W1TS/W1TC are
whole-port (bit p addresses pin p), so a thread holding the window can toggle the output
latch of ANY pin, not only its own -- but only pins the privileged shim ENABLED + muxed
actually drive a pad, and no other peripheral is reachable. PMP draws the per-thread line at
the **GPIO-block/register** level; single-pin isolation would need per-pin ETM/dedicated-GPIO
features (out of scope). This is the C6 twin of the F411 32 B window folding in I2SCFGR.

## APM open (the one-time background permit -- the piece the fleet has never driven)
APM registers are writable only in **TEE mode** (= M-mode), so this is a privileged/kernel
action, done ONCE before any U-mode driver runs. It is per-security-mode, NOT per-thread:
all REE0 U-threads share it. The minimal open, following 16.4:

1. U-mode security mode = **REE0**: `TEE_M0_MODE_CTRL_REG` (`0x6009_8000`) field `TEE_M0_MODE`
   -- reset 0 already selects REE0 for HP-CPU user mode, so confirm-only (no write needed if
   reset holds).
2. Leave region 0 as-is: reset `ADDR_START=0`, `ADDR_END=0xFFFFFFFF`, `ATTR=0` -- a catch-all
   that DENIES all REE modes everywhere. This is the background that keeps every other
   peripheral (IO MUX, PCR, the APM/TEE registers themselves) APM-closed to U-mode.
3. Program a dedicated permit region for the driver's peripheral block only -- recommend
   **region 1 over the GPIO Matrix slot** `0x6009_1000 .. 0x6009_1FFF`:
   - `HP_APM_REGION1_ADDR_START = 0x6009_1000`, `HP_APM_REGION1_ADDR_END = 0x6009_1FFF`
   - `HP_APM_REGION1_ATTR_REG`: `R0_R=1` (b2), `R0_W=1` (b1), `R0_X=0` (b0) -- REE0 read+write,
     no execute; REE1/REE2 left 0.
   - `HP_APM_REGION_FILTER_EN_REG |= (1<<1)` -- enable region 1.
   - `HP_APM_FUNC_CTRL_REG` already `0xF` (M0 enforcing) -- no change.

   Overlap semantics make this a permit-union: region 1 granting R/W over the GPIO block
   beats region 0's deny on the overlap ("if region 1 unreadable and region 2 readable, the
   overlap is readable", 16.3.2.3). So REE0 gains R/W to exactly the GPIO block while the
   catch-all keeps all other HP_PERI addresses APM-denied to U-mode.

**Where it lives:** a small TEE-mode chip helper (e.g. `kickos_c6_apm_open(base,end)`) called
from the driver's privileged bring-up shim -- mirroring how k64drv's shim clears the AIPS
`PACR SP` bit, and keeping `arch_init` lean (APM stays closed until a driver needs it). Note
it is process-global for REE0 regardless of driver count.

**Scoping alternatives (recommend the block-level above):**
- Tightest: region 1 = exactly `0x6009_1008 .. 0x6009_1010` (the 8 B PMP window). Minimal,
  but re-touches APM whenever the window moves.
- Broadest ("true background permit"): region 1 = all HP_PERI `0x6000_0000 .. 0x600A_FFFF`,
  REE0 R/W-NX. Cleanest conceptually (APM = coarse background, PMP = per-thread) and future
  drivers need no APM edit -- but it removes the APM belt-and-suspenders over the escalation
  surfaces (IO MUX/PCR), leaving only PMP between a U-thread and them. Block-level keeps the
  double lock over everything except the one granted block, so it is the safer minimal default.

Defense note: even the broad open cannot self-escalate -- APM config writes require TEE mode,
so a REE thread with a hypothetical PMP window over `0x6009_9000` still cannot reprogram APM.

## Privilege split (mirror k64drv / f411spi; the shim `main` runs privileged/TEE)
The unsafe one-time setup the unprivileged driver must NOT be able to do stays privileged and
OUT of the 8 B window: **PCR** (`0x6009_6000`, could ungate any peripheral), **IO MUX**
(`0x6009_0000`, could re-mux any pad), **GPIO_ENABLE / FUNC_OUT_SEL** (direction + routing),
and **APM/TEE** config. Keeping them out of the window is what makes the window a capability.

Privileged bring-up shim (once):
1. Ungate the GPIO/IO-MUX clocks in PCR as needed (the ROM leaves the GPIO matrix clocked;
   confirm at bench).
2. Configure the chosen pin as a push-pull output: `GPIO_ENABLE_W1TS = (1<<pin)`;
   `IO_MUX` pad = GPIO function (`MCU_SEL=1`) + a drive strength; route a simple GPIO output
   (or a constant) so the pad is driven by the output latch, not a peripheral signal. Program
   direction BEFORE the driver can touch the latch.
3. **APM open** (above) -- REE0 R/W over the GPIO block, no execute.
4. Spawn the UNPRIVILEGED driver: `mmio_base = 0x6009_1008`, `mmio_size = 8`,
   `privileged = false`, pow2-defined stack (`KOS_STACK_DEFINE`). No IRQ line.

## Transfer model: blink (no IRQ)
The driver loops: `*(W1TS) = (1<<pin)` (drive high), sleep, `*(W1TC) = (1<<pin)` (drive low),
sleep -- observable on the pin (LED/scope/logic analyzer; `GPIO_IN` readback is deliberately
outside the window, so verification is external, exactly as a blink is judged anyway). App
API stays faithful to "write a main, that's it"; the app never touches MMIO, grants, or the
APM. Then the negative test (below), which is terminal, is the LAST thing it does.

## The negative test (the per-thread isolation proof)
Announce-before-poke (k64drv idiom), then read/write an UNGRANTED peripheral register that is
**inside the APM-open block but outside the 8 B PMP window** -- recommend
**`GPIO_ENABLE` at `0x6009_1020`** (same GPIO block, APM-permitted for REE0, but no PMP entry
for this thread). On the C6 this MUST take a **store/load access fault**: PMP is checked
first and is fail-closed, so `mcause = 7` (store) / `5` (load), `mtval = 0x6009_1020`.
`kickos_rv_fault_report` (`arch_rv32imac.cc`) sees `from_user && mcause 5/7` and routes to
`kickos_isr_fault` -> the shared "MPU FAULT: task '<name>'" reported-fault path (same marker
as the sim SIGSEGV and the ARM MemManage). This is the sharpest possible proof: same
peripheral block, same APM background permit, yet the access faults **because PMP draws the
per-thread line at the 8 B window**. A second axis (poke a different peripheral entirely,
e.g. IO MUX `0x6009_0000`) also faults, but the same-block `ENABLE` poke isolates "PMP is the
per-thread discriminator" from "APM is the coarse background permit" cleanly.

**Critical semantic difference from PMP -- do NOT expect an APM denial to fault.** Per TRM
16.5, an APM-blocked access returns 0 on read / drops the write and raises a separate
`HP_APM_M0_INTR`; it does NOT trap `mcause 5/7`. So the isolation proof rides on the **PMP**
fault (checked first, per-thread), never on APM. An APM-only denial is silent at the
load/store and observable only via the `HP_APM_M0_EXCEPTION_*` registers. State this plainly
so a "no fault" from an APM-scope test is not misread as broken enforcement.

## What it proves (and the fleet contrast)
Per-thread peripheral isolation on the C6 = **PMP per-thread NAPOT window (granted blink
works; ungranted same-block poke faults `mcause 5/7`) layered over a one-time APM REE0
background open**. This is the RISC-V analog of the F411 canonical PMSA proof
(`design-spi-driver-stm32f411.md`), distinguished only by the extra APM open (F411 PMSA has
no second bus-side gate). Contrast with K64F: there the AIPS bridge is the sole, coarse,
per-4KB-slot/all-user gate and the SYSMPU window is inert -- no per-thread peripheral line
exists (k64drv). The C6 delivers the per-thread line K64F cannot, at the cost of the APM
background permit K64F does not need.

## Region / slot budget
Unprivileged driver PMP entries (`kickos_arch_mpu_commit` builds 8): app code (RX NAPOT) + app data
(RW) + private stack (RW) + GPIO MMIO (RW-NX, 8 B) = **4 of 8**. Comfortable. APM cost: one
region of 16 (region 0 reserved as the deny catch-all; region 1 = the GPIO block permit).

## Dependencies (state honestly) + sequencing
Gated, in order:
1. **C6 M2 PMP U-mode enforcement** -- build/enforcement-link validated, but currently
   **boot-loops on silicon** (whether the U-mode PMP trap fires from SRAM is the open
   on-silicon risk; a separate debug is in flight -- see `m2-readiness.md` C6 row). The driver
   cannot run until this is fixed; the negative test's `mcause 5/7` fault depends on it.
2. **The APM open** -- unwritten today; this brief scopes the minimal TEE-mode helper. Without
   it, even a correctly PMP-granted U-mode driver reaches nothing (default REE deny).
3. **The driver** -- privileged shim (clock/pin/APM-open/spawn) + unprivileged blink +
   terminal negative poke. Pure userspace addition given (1) and (2); no kernel change (the
   GPIO path has no IRQ).

Order: enforcement-boot fix -> APM open -> driver blink -> ungranted-poke PMP fault. Build
with `-DKICKOS_HAVE_MPU=1` (the app exists to prove enforcement). Build-only; the operator
flashes over the CH343P console, wires an LED/scope to the chosen pin, and validates.

## Open questions / risks
1. **Enforcement boot-loop (blocking).** No on-silicon result is possible until the C6 U-mode
   PMP-from-SRAM enforcement boots. Highest risk; owned by the separate M2 C6 debug.
2. **APM M-path + REE0 mapping** -- confirm HP CPU = HP_APM **M0** and `TEE_M0_MODE` reset (0)
   maps U-mode to REE0 on silicon (via `HP_APM_M0_EXCEPTION_*` + a controlled denial).
3. **Timer-IRQ follow-on needs an arch/chip change.** The C6 real-device IRQ dispatch is
   single-source (UART0 console, hard-wired `.Lextdev`). A timer (systimer / TIMG) userspace
   driver requires generalizing `kickos_rv_ext_dispatch_dev` to PLIC-claim-demux a logical
   line (or a second device CPU int + `switch.S` vector) so `kos_irq_register/wait/ack` reaches
   a REAL device, not just the software-inject doorbell. Then the W1C-before-re-arm storm rule
   (k64drv/f411 pattern; re-arm = next `kos_irq_wait`, or the optional `kos_irq_ack`) applies as usual. This is the reason GPIO is the first cut.
4. **Free pin unconfirmed** -- confirm the chosen GPIO is broken out + non-strapping against
   the DEV-KIT-N8 schematic before wiring.
5. **Whole-port W1TS/W1TC granularity** -- the PMP line is GPIO-block-level, not single-pin
   (a register limit). Documented; acceptable for the isolation proof.
6. **APM scope choice** -- block-level region 1 (recommended) vs whole-HP_PERI background
   permit vs exact-8 B; the block-level keeps the APM belt over the escalation surfaces.
