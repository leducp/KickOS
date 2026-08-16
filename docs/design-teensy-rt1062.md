<!-- SPDX-License-Identifier: CECILL-C -->
# Teensy 4.1 / i.MX RT1062 bring-up (spike)

> **Status: LANDED** -- the port shipped and runs on silicon. Kept as the DECISION record; the
> field values it used to carry -- the FCB/IVT/Boot-Data words and the LPUART6 wiring addresses --
> now live in `reference/boards.md` (*Per-board hardware facts*), which is code-synced. Three of
> the deferrals below have since closed: **MPU enforcement** works (and required a chip
> fixed-region wrap for the M7's speculative access -- `design-teensy-mpu-hang.md`), the **L1
> I-cache** is enabled as part of that fix, and the **L1 D-cache** is silicon-validated and now the
> imxrt default (`KICKOS_IMXRT_DCACHE` is ON in `arch/CMakeLists.txt`; the DMA coherency obligation
> arrives with M4-era DMA). Still deferred: the 600 MHz PLL tree (`SystemCoreClock` is the
> ROM-default 396 MHz) and the FlexRAM ITCM/DTCM split.

First-pass port to the PJRC Teensy 4.1 (NXP i.MX RT1062, Cortex-M7). Bounded scope: design plus
scaffold to a clean compile and link of a minimal image, with no bench available. Sources are
derived clean-room from the i.MX RT1060 Processor Reference Manual, Rev. 3 (cited as "RM x.y"); no
vendor SDK or HAL.

## Decisions

1. **Reuse the `armv7m` arch backend VERBATIM.** The M7 is ARMv7-M: same NVIC, SysTick,
   SVC trampoline, PendSV and fault path as the M3/M4 chips. This is a pure chip backend
   (`arch/arm/chip/imxrt1062/`) plus a board (`boards/teensy41/`).

2. **FlexSPI serial-NOR XIP image; writable state in OCRAM2.** HalfKay writes the image to the
   onboard QSPI flash and the boot ROM reads it from `0x6000_0000`, so a headerless TCM/OCRAM image
   is not deployable to a Teensy at all -- and there is no SWD bench either. The image must
   therefore carry the FlexSPI boot header regardless of where code runs, and given that, XIP is
   the simplest layout (VMA = LMA in flash, no `.text` copy). Its runtime reliability is exactly the
   FCB's reliability, which is the same dependency a copy-to-RAM image would have for its initial
   read, so non-XIP buys nothing and costs a copy step.

3. **OCRAM2, not DTCM, for `.data`/`.bss`/stacks/pool.** ITCM and DTCM are carved from the 512 KiB
   FlexRAM, whose split is set by `IOMUXC_GPR_GPR16`/`GPR17` from a fuse default at reset; OCRAM2 is
   a DEDICATED 512 KiB bank present at reset with no GPR or fuse dependency (RM 3.2). For an
   untestable first pass that removes an entire class of "what partition did the ROM leave" risk.
   DTCM/ITCM are a performance follow-up.

4. **The boot header must be STATIC CONST image data.** Any field needing a runtime initializer
   becomes a write to XIP flash and lands as 0 in the image. Caught during bring-up:
   `entry = &_boot_entry | 1` demoted the IVT to a dynamic initializer, so `entry` was 0. The Thumb
   LSB is already in the function-symbol relocation, so use a bare address constant.

5. **Set MSP in assembly before any C runs.** The boot ROM enters via `IVT.entry`, not the reset
   vector, and hardware does NOT load MSP from the vector table -- so `_boot_entry` (`startup.S`)
   sets `MSP = _estack` and `Reset_Handler` then sets `VTOR` to the relocated table.

6. **Verify the header layout at COMPILE time, not by inspection.** `offsetof`/`sizeof`
   `static_assert`s on the FCB structs, then confirmed once in the linked image.

7. **Single-pad `0x03` read at 30 MHz in the FCB read LUT.** The universally compatible read, with
   no quad-enable step, which is what makes a first image flashable without a bench to debug it on.

8. **DCD = NULL.** The ROM register defaults suffice with no SDRAM or SEMC, and RM Table 9-37 makes
   the DCD optional. One less untestable structure.

9. **No PLL bring-up on the first boot, and a deliberately LOW `SystemCoreClock`.** `clock_init()`
   is a no-op leaving the ROM clock. A wrong-low value only makes the monotonic clock run slow,
   never fast or unsafe, which is the safe direction for a board nobody can observe. (Now the
   ROM-default 396 MHz, read from the ROM's CCM tree rather than assumed.)

10. **Vector-table `.rept` count and the kernel IRQ table are ONE fact**, `KICKOS_MAX_IRQ`.

11. **Console = LPUART6 ("Serial1", pins 0/1), buffered TX over the shared ring** with a
    synchronous fallback for the panic path -- the same seam as `mk64f`, so the console behaves
    identically to the rest of the fleet from the first image.

## DEFERRED -- designed, not implemented

### 1. M7 PMSAv7 with per-region cache attributes -- PARTLY CLOSED

Enforcement landed (and needed `design-teensy-mpu-hang.md`'s fixed-region wrap for the M7's
speculative access), and the I-cache is enabled with it. What remains designed-not-built is
**per-region cacheability**. The M7 is the fleet's only cached core, so the RASR `TEX/S/C/B` fields,
which today encode one fixed policy per memory type, must become a per-region attribute:

- Current: `MEM_NORMAL = TEX=000,C=1,B=1` (write-back-no-write-allocate),
  `MEM_DEVICE = TEX=000,S=1,B=1`. Cacheless parts ignore C/B, so this was never exercised as a
  cache policy.
- Extension: a cacheability field on `struct arch_mpu_region` (or new `attr` flags, e.g.
  `ARCH_MPU_NC`) plus an encoder table -- Normal write-back write-allocate `TEX=001,C=1,B=1`,
  Normal NON-cacheable (DMA / shared buffers) `TEX=001,C=0,B=0`, Device `TEX=000,S=1,C=0,B=1`,
  Strongly-ordered `TEX=000,C=0,B=0`. The encodability check is unchanged; PMSAv7 geometry is
  identical on M3/M4/M7.
- **Cache enable is a SEPARATE concern from the MPU**, which is what let enforcement land before
  caches were on: until the cache is enabled the current `MEM_NORMAL` encoding is correct (it
  behaves non-cached). Clean/invalidate-by-MVA maintenance around DMA is driver-era work.
- Linker note: the `#if KICKOS_HAVE_MPU` app-data window in `imxrt1062.ld` must be pow2 base+size
  for PMSAv7, unlike K64F SYSMPU's 32-byte granularity; the mk64f 128 KiB `_appdata_size` is pow2
  and carries over.

### 2. 600 MHz clock tree
ARM PLL (PLL1) -> `CBCMR`/`CBCDR` -> core; **DCDC to the higher-OPP voltage FIRST** (the
voltage half of the wait-state ordering rule, `reference/porting.md`); `SystemCoreClock` from the
real root; proper UART and perclk roots selected. Baud tracks the real root once this lands.

### 3. FlexRAM -> DTCM/ITCM + XIP-to-ITCM hot code
Program `IOMUXC_GPR_GPR16`/`GPR17` for a deterministic split; move `.data`/`.bss`/stacks to DTCM and
copy latency-critical code to ITCM (a second load region), for real M7 speed.

### 4. Full C++ opt-in, and flashing
`kickos_cxx` leaf (exceptions/RTTI over newlib) as a later design-reviewed pass. Flashing is
HalfKay (`teensy_loader_cli` / Teensy Loader) with a `.hex`; the FCB read LUT plus `serialClkFreq`
and the UART clock root were the on-bench validate items.
