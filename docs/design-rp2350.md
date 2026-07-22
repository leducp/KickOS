<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# RP2350 bring-up (Cortex-M33) -- design spike

Terse, invariant-first. Register facts are clean-room from the RP2350 datasheet
RP-008373-DS-2 (section numbers cited inline and in the chip source). This pass
brings up the **Cortex-M33** only, to a clean compile+link; the **Hazard3
RV32IMAC(B)** core and the **PMSAv8 MPU** are designed here and implemented later.

Target board: Waveshare RP2350 Pi-Zero form factor (`boards/pizero2350`), 16 MiB
QSPI, UART0 console on GP0/GP1. Not flashed (no bench); build + image-inspection
verified. BOOTSEL-recoverable, so a wrong clock/boot config cannot brick it.

The M33 reuses the existing `armv7m` arch backend **verbatim** (armv8-m is a
superset of armv7-m for the thread/switch/NVIC/SVC/PendSV path; `switch.S`, the
BASEPRI critical section, and the SVC trampoline are unchanged). The chip adds only
the hardware edges: startup+vectors, the IMAGE_DEF boot block, clock tree, UART0
console, and the linker script.

```
                 arch/arm/armv7m/  (reused, untouched)
                        |
   arch/arm/chip/rp2350/  <-- this pass
     startup.S      vectors (pinned @ 0x1000_0000) + IMAGE_DEF block
     chip_rp2350.cc clocks/PLL/TICKS/UART0/console/reset + Reset_Handler
     rp2350.ld      flash/SRAM map, vector pin, C-runtime tables
     include/kickos/board_config.h   KICKOS_MAX_IRQ=52 + stacks
```

## Memory map (datasheet 2.2)

| Region | Base | Size | Notes |
|--------|------|------|-------|
| XIP / QSPI flash | `0x1000_0000` | 16 MiB (this board) | image runs in place; bootrom does XIP setup |
| SRAM (striped) | `0x2000_0000` | 512 KiB | main RAM used here; striped on addr[3:2] over 8 banks |
| SRAM8/SRAM9 | `0x2008_0000` / `0x2008_1000` | 4 KiB each | non-striped; **reserved** (future per-core stacks) |
| SIO (core-local) | `0xd000_0000` | -- | GPIO bit-bang (offsets shifted vs RP2040) |

Total SRAM is 520 KiB; the two non-striped 4 KiB banks are left out of the linear
RAM region for now (datasheet 2.2.3 recommends them for hot data / core stacks).
`_estack = 0x2008_0000`; the top 8 KiB is the kernel MSP stack.

**All APB peripheral bases moved vs RP2040** (datasheet 2.2.4). Recomputed in the
chip source: RESETS `0x4002_0000`, CLOCKS `0x4001_0000`, XOSC `0x4004_8000`,
PLL_SYS `0x4005_0000`, IO_BANK0 `0x4002_8000`, PADS_BANK0 `0x4003_8000`,
UART0 `0x4007_0000`, TIMER0 `0x400b_0000`, TICKS `0x4010_8000`.

## Boot: IMAGE_DEF block + vector-table pin (the load-bearing invariant)

The RP2040 hazard was the 256-byte CRC-checked boot2 at flash 0 displacing the
vector table to `0x1000_0100` -- get the offset wrong and the ROM jumps into
garbage. The RP2350 **removes boot2 entirely** (datasheet 5.9.5): the bootrom does
its own best-effort QSPI XIP setup, then scans the **first 4 KiB** of the flash
image (5.1.5.2) for a **block loop** containing a valid **IMAGE_DEF**. The new
hazard is the reincarnation of the same class -- the block must exist, be
well-formed, and be inside the 4 KiB window, or the image is rejected.

**Control transfer** (5.9.3.3, 5.9.5.1): with no `ENTRY_POINT` and no
`VECTOR_TABLE` item, the bootrom takes the Arm vector table to be **at the image
base**, and enters via the initial SP at `[base+0]` and reset PC at `[base+4]`. It
sets Secure SP + VTOR before entry (5.2.2).

**Our guarantee of the vector VMA** -- same lever as `rp2040.ld` (pin, don't
merely assert): `rp2350.ld` places `.text` at `ORIGIN(FLASH)` with
`KEEP(*(.isr_vector))` first, so the vector table *is* the image base. The
IMAGE_DEF block is `KEEP(*(.image_def))` immediately after it (lands at
`0x1000_0110`, well inside 4 KiB). Two ASSERTs enforce both: `ADDR(.text) ==
ORIGIN(FLASH)` and `g_image_def < ORIGIN(FLASH)+0x1000`. Reset_Handler also writes
`SCB->VTOR = 0x1000_0000` explicitly (robust against a warm-reboot/debugger entry
that skipped the bootrom).

**Force-keep without a shared CMake edit:** nothing references the block. It rides
into the link inside `startup.o`, which is already force-pulled by the arm-family
`-Wl,-u,g_isr_vector` (top `CMakeLists.txt`); `KEEP` then protects `.image_def`
from `--gc-sections`. No boot2 checksum tool, no `-u` addition -- unlike RP2040.

**The 20-byte block** (datasheet 5.9.5.1 "Minimum Arm IMAGE_DEF", emitted verbatim
in `startup.S`):

```
0xffffded3   PICOBIN_BLOCK_MARKER_START
0x10210142   IMAGE_DEF item: type=0x42, size=1 word, image_type_flags=0x1021
0x000001ff   LAST item: covered size = 1 word
0x00000000   LINK: 0 = self-loop (single-block loop)
0xab123579   PICOBIN_BLOCK_MARKER_END
```

`image_type_flags = 0x1021` = EXE(1) | Security **S**(2) | CPU **ARM**(0) | CHIP
**RP2350**(1) (5.9.3.1). Security=S matches the state the M33 boots in (no
TrustZone, we never drop to Non-secure). "Unsigned" is orthogonal to this field: on
a chip that is not secured (`CRIT1.SECURE_BOOT_ENABLE` clear) **no SIGNATURE/
HASH_DEF item is required** and this block is accepted as-is (5.9.5). NS or
Security-UNSPECIFIED variants (`0x1011` / `0x1001`) exist if a future TrustZone
split needs them.

Verified in the linked `hello`: word0=`0x2008_0000` (SP), word1=`0x1000_0af9`
(Reset_Handler|thumb), `g_image_def`@`0x1000_0110` decoding to the five words
above.

## Clock plan (datasheet 8.x)

Bring-up order (`clocks_init`, mirroring the RP2040 discipline -- every poll
bounded, dead-crystal degrades instead of hanging):

1. XOSC: program `FREQ_RANGE=0xaa0` (12 MHz) then `ENABLE=0xfab`, poll `STATUS.STABLE`.
2. `clk_ref <- XOSC` (glitchless), poll one-hot `CLK_REF_SELECTED`.
3. **TICKS TIMER0 generator** (8.5, the new common tick block -- *not* the watchdog
   tick of RP2040): `CYCLES=12`, `ENABLE=1` -> 1 MHz. This is the source for the
   64-bit system TIMER0. Kept on `clk_ref` so the monotonic clock is PLL-independent.
4. PLL_SYS to **150 MHz** (8.6 default max): `12 MHz /REFDIV=1 xFBDIV=125 = 1500 MHz
   VCO /POSTDIV1=5 /POSTDIV2=2`. Poll `CS.LOCK`.
5. `clk_sys <- PLL` (AUXSRC then SRC, poll SELECTED); set `SystemCoreClock=150e6`
   in the same step (SysTick ns<->cycle math reads it).
6. `clk_peri <- clk_sys`; recompute UART divisors for 150 MHz.

Fallbacks: no XOSC -> stay on ROSC (~6.5 MHz, TICKS CYCLES=7), `SystemCoreClock`
lowered. No PLL lock -> stay on clk_ref 12 MHz, UART on XOSC. The board always
reaches a console.

**Monotonic clock** = 64-bit TIMER0 microsecond counter (12.8), read via the
non-latching `TIMERAWH/L` halves with a hi/lo/hi re-read (core-safe). Overrides the
arch's weak DWT default -- TIMER0 is a true 64-bit source, no 32-bit wrap. TIMER0
only counts once the TICKS TIMER0 generator (step 3) runs.

## UART0 console (datasheet 12.1)

Standard ARM PL011 at `0x4007_0000` (register offsets identical to RP2040). GP0=TX,
GP1=RX via `FUNCSEL=2`. **RP2350 pad gotcha:** pads reset **ISOLATED** (`PADS.ISO`
bit 8 set) -- `uart0_init` clears ISO on both pins (plus OD on TX, sets IE on RX),
which the RP2040 did not need. FIFO left disabled (FEN=0) so the buffered
console-TX ring's idle->busy prime re-triggers the drain (the fleet pattern; at-rest
TXRIS assertion is HW-unverified, build-only board). Baud 115200: IBRD/FBRD =
81/24 at 150 MHz, 6/33 at 12 MHz. UART0 reset is released **after** `clocks_init`
(it is on `clk_peri`; releasing before the clock is live hangs on RESET_DONE -- the
RP2040 lesson).

Diagnostic LED: **omitted**. The Waveshare Pi-Zero exposes only a WS2812 RGB LED
(needs a PIO/bit-bang protocol, not a simple GPIO level). `arch_diag_led_*` keep
their weak no-op defaults; a real WS2812 driver is a later, driver-era concern.

## Interrupts

52 NVIC inputs (3.2): IRQ0..51, of which 46..51 are spare and never fire.
`KICKOS_MAX_IRQ=52` sizes both the `startup.S` vector `.rept` and the kernel IRQ
table from one fact. UART0_IRQ = 33.

---

## DEFERRED (a): armv8-m / PMSAv8 MPU backend

Not built this pass; `KICKOS_HAVE_MPU=0`. The Cortex-M33 MPU is **PMSAv8**, which
is materially better than the v6-M/v7-M PMSA the fleet uses today, and the seam
already fits it: `arch_mpu_apply` stays the shared stash, a PMSAv8 backend overrides
only `kickos_arch_mpu_commit` (+ `arch_mpu_region_encodable`); the region-set contract
in architecture.md treats `attr` as unprivileged rights, which PMSAv8 honors.

- **RBAR/RLAR base+limit, not base+size-pow2.** A region is `[BASE, LIMIT]` at
  32-byte granularity (RBAR bits[31:5]=base, RLAR bits[31:5]=limit, EN in RLAR).
  So a domain's data/stack region is an **arbitrary, non-power-of-two range** --
  no pow2 padding, no size-alignment. This **deletes the pow2 `.appdata` window
  machinery** that `rp2040.ld` needs (v6/v7 PMSA require size==pow2 and base
  aligned to size). The rp2350 enforcement `.ld` will be *simpler*: kernel
  data/bss and an app data/bss range each described by two symbols (start,end)
  fed to RBAR/RLAR directly.
- **MAIR attributes.** PMSAv8 splits memory *type* from *permissions*: RLAR.AttrIndx
  selects one of 8 MAIR0/MAIR1 attribute slots (Normal-WBWA for SRAM/flash, Device-nGnRE
  for MMIO). `ARCH_MPU_DEV` maps to a Device MAIR slot + XN -- meaningful here (like
  ARM v7 PMSA, unlike PMP/RX which are R/W/X only).
- **Background region / PRIVDEFENA** unchanged from v7: the kernel is the degenerate
  privileged domain reached via the background map, spending ~0 explicit regions.
- **8 regions** (MPU_TYPE.DREGION; M33 typically 8). Budget identical to the fleet:
  code(RX) + data(RW-NX) + optional MMIO + per-thread stack + guard, within 8.
- Work: a strong `kickos_arch_mpu_commit` override (RBAR/RLAR/MAIR encode) in a small
  `armv8m`-specific TU that reads the shared stash; `arch_mpu_region_encodable` becomes
  trivial (any 32-byte-aligned range encodes). Silicon proof: the `mpu_fault` selftest
  (ungranted write faults MemManage). (Now built via that commit override -- see
  `design-rp2350-mpu-armv8m.md`.)

## DEFERRED (b): Hazard3 RV32IMAC(B) target + the ~70% shared layer

The RP2350 is dual-arch: an M33 pair **and** a Hazard3 RISC-V pair select the same
boot. A `hazard3` target reuses the existing `arch/riscv/rv32imac` backend (mtvec
demux, CLINT msip deferred switch, ecall trampoline, PMP) -- the C6/virt model --
with a new `arch/riscv/chip/rp2350_riscv` (name TBD) chip layer. Hazard3 adds the
**B** (bit-manip) extension; harmless to the arch (ISA superset), a `-march` knob at
most.

**Boot is shared, with a one-word delta:** the same IMAGE_DEF mechanism, but
`image_type_flags` CPU field = **RISC-V(1)** instead of ARM(0) (`0x1121` vs
`0x1021`). The bootrom enters the RISC-V core at the image and reads the entry the
RISC-V way (no Arm vector table; an `ENTRY_POINT` item or the RISC-V reset
convention -- to be pinned against datasheet 5.9.3.4 when built). Everything else in
the block layout is identical.

**~70% shared between the two chip backends** (the argument for factoring, not
copy-paste, when the second lands): the clock tree (XOSC/PLL_SYS/CLOCKS/TICKS
sequencing), the reset-release ordering, UART0 PL011 + the console-TX backend,
GPIO/PADS (including the ISO-bit gotcha), and the 64-bit TIMER0 clock are all
**core-agnostic** -- identical registers, identical sequences. Only the CPU-facing
pieces differ: vector table vs mtvec, VTOR/CPACR vs the RISC-V CSR init, the MPU
(PMSAv8) vs PMP, and the IMAGE_DEF CPU flag. Plan: when the Hazard3 target is built,
lift the shared clock/UART/GPIO/TIMER code into a common `arch/arm/chip/rp2350/`-
adjacent unit both chips link (or a shared header of `static inline` register
sequences), rather than duplicating it. This pass keeps it all in `chip_rp2350.cc`;
the split is cheap and better motivated once the second consumer exists.

## Also deferred

- **Full C++ under enforcement** (`kickos_cxx`, `-fexceptions/-frtti`): the EHABI
  `.ARM.exidx/.extab/.gcc_except_table` are already homed in flash by `rp2350.ld`,
  so libstdc++/libsupc++ over newlib links cleanly on the M33 later (EHABI, as on
  the other ARM parts). Needs the PMSAv8 `.appdata` window (deferred (a)) for the
  isolated heap.
- **UF2 emission / flashing**: `.bin` + `.hex` are emitted today; a `.uf2` needs the
  RP2350 family-id and the picotool/uf2 packer -- add when there is bench access.
- **Second core (core1), WS2812 LED, real peripheral IRQ receive**: driver-era.
```
