<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# RP2350 bring-up (Cortex-M33) -- design spike

> **Status: LANDED** -- the Cortex-M33 port shipped and runs on silicon, and the PMSAv8 MPU it
> deferred landed too (`design-rp2350-mpu-armv8m.md`). Kept as the DECISION record; the register
> facts it used to carry -- the IMAGE_DEF block and its `image_type_flags` decode, the recomputed
> APB bases, the clock recipe, the `PADS.ISO` gotcha and the IRQ numbers -- now live in
> `reference/boards.md` (*Per-board hardware facts*), which is code-synced. The **Hazard3
> RV32IMAC(B)** core remains unimplemented (`design-rp2350-hazard3.md`, EXPLORATORY).

Register facts were derived clean-room from the RP2350 datasheet RP-008373-DS-2 (section numbers
cited in the chip source). This pass brought up the **Cortex-M33** only; the Hazard3 core and the
PMSAv8 MPU were designed here and implemented later.

Target board: Waveshare RP2350 Pi-Zero form factor (`boards/pizero2350`), 16 MiB QSPI.
BOOTSEL-recoverable, so a wrong clock or boot config cannot brick it.

## Decisions

1. **Reuse the `armv7m` arch backend VERBATIM.** ARMv8-M is a superset of ARMv7-M for everything
   the arch layer touches, so `switch.S`, the BASEPRI critical section and the SVC trampoline are
   unchanged. The chip adds only the hardware edges: startup + vectors, the IMAGE_DEF block, the
   clock tree, the console and the linker script. Only the MPU differs, and that is a separate
   backend selected by presence-in-link (`design-rp2350-mpu-armv8m.md`).

2. **Console on UART1 / GP4-GP5, not UART0 / GP0-GP1.** This document originally said UART0; the
   Pi-Zero header does not bring those pins out. The shipped port uses UART1
   (`arch/arm/chip/rp2350/chip_rp2350.cc`).

3. **PIN the vector table's VMA, do not merely ASSERT it.** Same lever as `rp2040.ld`: `.text` at
   `ORIGIN(FLASH)` with `KEEP(*(.isr_vector))` first, so the vector table IS the image base the
   bootrom enters through, and the IMAGE_DEF block sits immediately after it. `Reset_Handler` also
   writes `SCB->VTOR` explicitly, which is what makes a warm reboot or a debugger entry that
   skipped the bootrom safe. The RP2040 hazard -- a CRC-checked boot2 displacing the vector table
   -- is gone with boot2 itself, and the replacement hazard is the same class: the block must
   exist, be well-formed, and be inside the first 4 KiB.

4. **Force-keep the boot block WITHOUT a shared CMake edit.** Nothing references it, but it rides
   into the link inside `startup.o`, already force-pulled by the arm-family `-Wl,-u,g_isr_vector`,
   and `KEEP` then protects `.image_def` from `--gc-sections`. So there is no checksum tool and no
   `-u` addition -- unlike RP2040.

5. **Leave SRAM8/SRAM9 out of the linear RAM region.** The two non-striped 4 KiB banks
   (`0x2008_0000` / `0x2008_1000`) are reserved for future per-core stacks and hot data, which is
   what datasheet 2.2.3 recommends them for. `_estack = 0x2008_0000`; the top 8 KiB is the kernel
   MSP stack.

6. **Keep the monotonic clock PLL-independent.** The 64-bit TIMER0 is fed by the TICKS generator
   on `clk_ref`, so a PLL change cannot move it and `arch_clock_now` needs no re-anchor. Read via
   the non-latching `TIMERAWH`/`TIMERAWL` halves with a hi/lo/hi re-read (core-safe). The chip also
   defines `arch_trace_now` (`TIMERAWL`), displacing the armv7m DWT `CYCCNT` fallback, which does
   not exist usefully here.

7. **Every clock poll is BOUNDED and degrades instead of hanging.** No XOSC leaves ROSC with a
   lowered `SystemCoreClock`; no PLL lock stays on `clk_ref` with the UART on XOSC. The board
   always reaches a console.

8. **Release UART reset AFTER `clocks_init`.** It is on `clk_peri`, and releasing before that
   clock is live hangs forever on `RESET_DONE` -- the RP2040 lesson, unchanged here.

9. **Leave the UART FIFO disabled (`FEN = 0`).** The buffered console-TX ring's idle-to-busy prime
   is what re-triggers the drain, which is the fleet pattern; at-rest `TXRIS` assertion was never
   hardware-verified.

10. **Omit the diagnostic LED.** The Waveshare Pi-Zero exposes only a WS2812 RGB LED, which needs a
    PIO or bit-bang protocol rather than a GPIO level, so `arch_diag_led_*` keep their no-op
    fallbacks. A real WS2812 driver is a driver-era concern.

11. **Size the vector table and the kernel IRQ table from ONE fact**, `KICKOS_MAX_IRQ`, so the
    `startup.S` `.rept` and the kernel table cannot skew.

---

## DEFERRED (a): armv8-m / PMSAv8 MPU backend -- LANDED

Built as `arch/arm/common/arch_arm_pmsav8.cc`, opted into by
`arch/arm/chip/rp2350/mpu.cmake`. The decision and the register encoding are
`design-rp2350-mpu-armv8m.md`; the contract is `reference/porting.md` (*MPU descriptor
encodings*). The payoff this document predicted did land: because RLAR takes an arbitrary limit,
the pow2 `.appdata` window machinery `rp2040.ld` needs is not required, and the region-set contract
in `architecture.md` already treated `attr` as unprivileged rights, which PMSAv8 honors.

## DEFERRED (b): Hazard3 RV32IMAC(B) target + the ~70% shared layer

The RP2350 is dual-arch: an M33 pair **and** a Hazard3 RISC-V pair select the same boot. A
`hazard3` target would reuse the existing `arch/riscv/rv32imac` backend (mtvec demux, CLINT msip
deferred switch, ecall trampoline, PMP) -- the C6/virt model -- with a new chip layer. Hazard3 adds
the **B** extension, which is harmless to the arch (ISA superset) and a `-march` knob at most.

**Boot is shared, with a one-word delta:** the same IMAGE_DEF mechanism with the
`image_type_flags` CPU field set to RISC-V instead of ARM. The bootrom enters the RISC-V core at
the image and reads the entry the RISC-V way (no Arm vector table; an `ENTRY_POINT` item or the
RISC-V reset convention, to be pinned against datasheet 5.9.3.4 when built). Everything else in the
block layout is identical.

**~70% is shared between the two chip backends**, which is the argument for factoring rather than
copy-pasting when the second lands: the clock tree (XOSC/PLL_SYS/CLOCKS/TICKS sequencing), the
reset-release ordering, the PL011 console plus its TX backend, GPIO/PADS including the ISO-bit
gotcha, and the 64-bit TIMER are all **core-agnostic** -- identical registers, identical sequences.
Only the CPU-facing pieces differ: vector table vs mtvec, VTOR/CPACR vs the RISC-V CSR init, PMSAv8
vs PMP, and the one IMAGE_DEF CPU flag. Plan: lift the shared clock/UART/GPIO/TIMER code into a
unit both chips link (or a header of `static inline` register sequences) when the second consumer
exists, rather than duplicating it. This pass keeps it all in `chip_rp2350.cc`; the split is cheap
and better motivated once there is something to share it with.

## Also deferred

- **Full C++ under enforcement** (`kickos_cxx`, `-fexceptions`/`-frtti`): the EHABI
  `.ARM.exidx`/`.extab`/`.gcc_except_table` are already homed in flash by `rp2350.ld`, so
  libstdc++/libsupc++ over newlib links cleanly on the M33.
- **UF2 emission**: `.bin` + `.hex` are emitted today; a `.uf2` needs the RP2350 family-id and the
  picotool/uf2 packer.
- **Second core (core1), the WS2812 LED, and a real peripheral IRQ receive**: driver-era.
