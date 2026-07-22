<!-- SPDX-License-Identifier: CECILL-C -->
# Teensy 4.1 / i.MX RT1062 bring-up (spike)

First-pass port of KickOS to the PJRC Teensy 4.1 (NXP i.MX RT1062, Cortex-M7).
Bounded scope: **design + scaffold to a clean compile+link of a minimal image.**
Not flashed (no bench), no MPU, no L1 cache, no PLL bring-up -- those are DEFERRED
(designed below, not implemented). Sources are derived clean-room from the i.MX
RT1060 Processor Reference Manual, Rev. 3 (cited as "RM x.y"); no vendor SDK/HAL.

The M7 is ARMv7-M: same NVIC / SysTick / SVC-trampoline / PendSV / fault path as
the M3/M4 chips, so the `armv7m` arch backend is reused **verbatim**. This port is
a pure chip backend (`arch/arm/chip/imxrt1062/`) plus a board (`boards/teensy41/`).

## Invariants (what must hold)

- The boot ROM enters via **IVT.entry**, NOT the reset vector. Hardware does not
  load MSP from the vector table, so `_boot_entry` (startup.S) sets MSP=`_estack`
  before any C runs, and `Reset_Handler` sets `VTOR` to the relocated table.
- The three ROM-consumed structures live at fixed flash offsets: FCB @ 0x0000,
  IVT @ 0x1000, Boot Data @ 0x1020 (RM Table 9-36: FlexSPI-NOR IVT offset 0x1000).
- The boot header must be **static (const) image data** -- any field needing a
  runtime initializer would be a write to XIP flash and land as 0 in the image.
  (Caught during bring-up: `entry = &_boot_entry | 1` demoted the IVT to a dynamic
  initializer -> entry=0. Fix: the Thumb LSB is already in the function-symbol
  relocation; use a bare address constant.)
- Vector-table `.rept` count and the kernel IRQ table are one fact: `KICKOS_MAX_IRQ`.

## Boot + memory map (the decision)

**Decision: FlexSPI serial-NOR XIP image; writable state in OCRAM2.**

HalfKay (the Teensy loader) writes the image to the onboard QSPI flash and the
boot ROM reads it from **0x6000_0000** -- a headerless TCM/OCRAM image is not
deployable to a Teensy at all (no SWD bench either). So the image *must* carry the
FlexSPI boot header regardless of where code runs. Given that, XIP is the simplest
linker layout (VMA=LMA in flash, no `.text` copy) and its runtime reliability is
exactly the FCB's reliability -- the same dependency a non-XIP "copy to RAM" image
would have for its initial read. Non-XIP buys nothing here and costs a copy step.

Memory map (RM 3.2, Table 3-1):

| Region | Address | Use in this port |
|--------|---------|------------------|
| FlexSPI NOR (XIP) | 0x6000_0000 (8 MiB, Teensy W25Q64) | `.text`/`.rodata` (XIP), `.data` LMA |
| ITCM  | 0x0000_0000 | unused (FlexRAM; DEFERRED: hot-code copy) |
| DTCM  | 0x2000_0000 | unused (FlexRAM; DEFERRED: fast data) |
| OCRAM2 | 0x2020_0000 (512 KiB) | `.data`/`.bss`/stacks/RAM pool |

`.data` is loaded from its flash LMA and `.bss` zeroed by the generic
`kickos_ranges_init` (the copy/zero tables in the linker script).

**Why OCRAM2 and not DTCM.** ITCM/DTCM are carved from the 512 KiB **FlexRAM**,
whose ITCM/DTCM/OCRAM split is set by `IOMUXC_GPR_GPR16/GPR17` (fuse default at
reset). OCRAM2 is a **dedicated** 512 KiB bank, present at reset with no GPR/fuse
dependency (RM 3.2). For an untestable first pass that removes an entire class of
"what partition did the ROM leave" risk. DTCM/ITCM are a performance follow-up.

### Boot header formats used (RM chapter 9)

- **FCB** (RM 9.6.3, Table 9-15/9-18), 512 B @ flash+0: tag `'FCFB'`, version
  `0x56010000`, `deviceType=1` (serial NOR), `sflashPadType=1` (**single pad**),
  `serialClkFreq=1` (30 MHz), `sflashA1Size=8 MiB`. Read LUT (seq 0) = 1-1-1
  `0x03` normal read + 24-bit address (`lookupTable[0]=0x08180403`,
  `[1]=0x00002404`; LUT instr = `(opcode<<10)|(pads<<8)|operand`, RM 9.6.3.1).
  Single-pad 0x03 @ 30 MHz is the universally-compatible read (no quad-enable),
  chosen for an untestable first image.
- **IVT** (RM 9.7.1, Table 9-37), 32 B @ flash+0x1000: header `0x412000D1`
  (tag 0xD1, len 0x20, version 0x41), `entry=&_boot_entry`, `dcd=0`,
  `boot_data=&Boot Data`, `self=0x6000_1000`, `csf=0`.
- **Boot Data** (RM 9.7.1.2, Table 9-38) @ flash+0x1020: `start=0x6000_0000`,
  `length` = on-flash image extent (linker `__boot_image_length`), `plugin=0`.
- **DCD = NULL**: ROM register defaults suffice (no SDRAM/SEMC). Optional per RM
  Table 9-37.

Layout is self-verified by `offsetof`/`sizeof` static_asserts on the FCB structs
and confirmed in the linked image (`.boot_fcb`@0x6000_0000, `.boot_ivt`@0x6000_1000,
`.isr_vector`@0x6000_2000).

## Reset + clock plan

`_boot_entry` (asm): set MSP=`_estack`, branch to `Reset_Handler`.
`Reset_Handler` (C): enable FPU (CPACR CP10/11), set `VTOR`, run
`kickos_ranges_init` + ctors, `arch_init`, `kmain`.

`arch_init` -> `clock_init()` (**no-op**, leaves the ROM clock) -> `uart6_init` ->
`kickos_armv7m_init` (installs SVCall/PendSV/SysTick priorities, starts DWT).

**Clock (conservative first boot).** No PLL bring-up. `SystemCoreClock` is a
PLACEHOLDER `24_000_000` (the XTAL floor): a wrong-low value only makes the DWT
monotonic clock run slow, never fast/unsafe. The 600 MHz target (ARM PLL / CCM
roots, DCDC voltage for the higher OPP) is a follow-up; when it lands,
`SystemCoreClock` is set from the real core-clock root.

## Console: LPUART6 = Teensy "Serial1" (pins 0/1)

Teensy Serial1 is **LPUART6** (RM Table 4-2: LPUART6 = IRQ 25). Pins 0/1 = RX/TX
on pads `GPIO_AD_B0_03`/`GPIO_AD_B0_02`, ALT2 = LPUART6_RX/_TX (RM ch.11):

- clock gate: `CCM_CCGR3` CG3 (0x400F_C074) -- already enabled at reset.
- pin mux: `IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B0_02/03` = ALT2 (0x401F_80C4/0x80C8);
  RX daisy `IOMUXC_LPUART6_RX_SELECT_INPUT`.
- LPUART6 @ 0x4019_8000 (RM Table 3-3): GLOBAL/BAUD/STAT/CTRL/DATA at 0x08/0x10/
  0x14/0x18/0x1C. Buffered TX via the shared `console_tx` ring (drained by the
  LPUART6 TX-empty IRQ), synchronous fallback for the panic path -- same seam as
  mk64f.

Baud assumes the reset UART clock root (`pll3_80m/1 = 80 MHz`, RM 14); OSR=15,
SBR=`root/(baud*16)`. Baud tracks the real root once the CCM bring-up lands.

## DEFERRED -- designed, not implemented

### 1. M7 MPU (PMSAv7) with cache attributes

The shared `armv7m` PMSA backend (`arch_arm_common.cc`: `kickos_arm_mpu_program`, run from the
weak `kickos_arch_mpu_commit`, + `arch_mpu_region_encodable`) already programs pow2 regions and
RASR AP/XN. The M7
difference the roadmap flagged: **the M7 has L1 I/D cache; every existing chip is
cacheless**, so the RASR `TEX/S/C/B` fields, which today encode one fixed policy
per memory type, must become **per-region cache attributes**:

- Current: `MEM_NORMAL = TEX=000,C=1,B=1` (write-back-no-write-allocate),
  `MEM_DEVICE = TEX=000,S=1,B=1` (shareable device). Cacheless parts ignore C/B,
  so this was never exercised as a cache policy.
- Extension: add a cacheability field to `struct arch_mpu_region` (or new `attr`
  flags, e.g. `ARCH_MPU_NC`) and an encoder table:
  - Normal write-back write-allocate: `TEX=001,C=1,B=1`.
  - **Normal non-cacheable** (DMA / shared buffers): `TEX=001,C=0,B=0`.
  - Device: `TEX=000,S=1,C=0,B=1`. Strongly-ordered: `TEX=000,C=0,B=0`.
  The pow2/base-alignment encodability check is unchanged (PMSAv7 geometry is
  identical on M3/M4/M7).
- Cache is a **separate enable** from the MPU: the M7 D/I cache invalidate-then-
  enable sequence (CCR.IC/DC; set/way invalidate via CSSELR/CCSIDR, M7 TRM) and
  clean/invalidate-by-MVA maintenance around DMA are driver-era work. Until the
  cache is enabled, the current `MEM_NORMAL` encoding is correct (behaves
  non-cached). So MPU enforcement can land (M2) *before* caches are turned on.
- Linker note: the `#if KICKOS_HAVE_MPU` app-data window in `imxrt1062.ld` must be
  pow2 base+size for PMSAv7 (unlike K64F SYSMPU's 32-byte granularity); the mk64f
  128 KiB `_appdata_size` is pow2 and carries over.

### 2. 600 MHz clock tree
ARM PLL (PLL1) -> `CBCMR`/`CBCDR` -> core; DCDC to the higher-OPP voltage first;
`SystemCoreClock` from the real root. Also select proper UART/perclk roots.

### 3. FlexRAM -> DTCM/ITCM + XIP-to-ITCM hot code
Program `IOMUXC_GPR_GPR16/GPR17` for a deterministic ITCM/DTCM split; move
`.data`/`.bss`/stacks to DTCM and copy latency-critical code to ITCM (a second
load region), for real M7 speed.

### 4. Full C++ opt-in, and flashing
`kickos_cxx` leaf (exceptions/RTTI over newlib) as a later design-reviewed pass.
Flashing is HalfKay (`teensy_loader_cli` / Teensy Loader) with a `.hex`; the FCB
read LUT + `serialClkFreq` and the UART clock root are the on-bench validate items.

## Build

```
cmake --preset teensy41           # or teensy41-st (selftest)
cmake --build build/teensy41
```

Link-clean (arm-none-eabi 15.3.rel1, `-mcpu=cortex-m7 -mfpu=fpv5-d16
-mfloat-abi=softfp`). `hello`: text 22296 / data 100 / bss 18644. Boot header +
sections verified with `objdump -h -s`.
