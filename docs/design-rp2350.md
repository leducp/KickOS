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
   on `clk_ref`, so a PLL change cannot move it and `arch_clock_now` needs no re-anchor. `clk_ref`
   is the source divided by `CLK_REF_DIV`, which the bootrom leaves at 4, so the generator's
   `CYCLES` is derived from that divisor rather than from the crystal frequency. Read via
   the non-latching `TIMERAWH`/`TIMERAWL` halves with a hi/lo/hi re-read (core-safe). The chip also
   defines `arch_trace_now` (`TIMERAWL`), displacing the armv7m DWT `CYCCNT` fallback, which does
   not exist usefully here.

7. **Every clock poll is BOUNDED and degrades instead of hanging.** Neither a dead XOSC nor a
   dead PLL moves `clk_sys` off the ROSC the bootrom parked it on, so `SystemCoreClock` takes the
   ROSC nominal in both; the dead-PLL case still has a crystal, so the UART goes on XOSC. The
   board always reaches a console.

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
  picotool/uf2 packer. **AND THE REASON IT STAYS DEFERRED IS PROVENANCE RATHER THAN EFFORT.** The
  datasheet gives the family ids (Table 455, section 5.5.3: `rp2350_arm_s` is `0xe48bff59`), the
  256-byte `payload_size` and the target alignment, and it does NOT document the UF2 block
  layout at all: no 512-byte block size, no 32-byte header, no field offsets. An emitter
  therefore needs the Microsoft UF2 specification, which is a different provenance from the
  clean-room fact source this port is written against. A container we cannot source is not a
  stronger witness than one we can, and `picotool load -x -t elf` takes the ELF today, which is
  what every other board in the fleet is flashed as.
- **Second core (core1), the WS2812 LED, and a real peripheral IRQ receive**: driver-era.

## The AMP partition on this part, and facts a port written blind would miss

**EVERY NODE PINS `SCB->VTOR` FROM `g_isr_vector` AND NEVER FROM A LITERAL, and the literal cost
a milestone.** `Reset_Handler` used to write `0x1000_0000`. Every node compiles that line and a
peer links at its own flash slice, so a peer pointed its vector table at NODE 0's, and the launch
handshake had already handed core 1 the right address for it to throw away. From its second
instruction on, every exception the peer took vectored into node 0's image and ran node 0's
handlers against node 0's SRAM: `switch.S` wrote the other core's PSP into a node 0 thread's
`ctx.sp` -- `ctx` being `Thread`'s first member, so that store lands on the first word of a TCB --
and both cores raced `g_arch_current` and `g_arch_next` with no lock, `kickos_kernel_core()`
folding to `0u` on this posture. `VTOR` is core-local, so nothing outside the peer can read it;
the peer must report its own.

**AND THE FAULT THAT DEFECT PRESENTS AS NAMES AN ADDRESS THE TREE CANNOT PRODUCE.** Node 0 takes a
bus fault at `PendSV_Handler` on the incoming context restore with `R0` and `BFAR` both reading
`0xf000_0000`. That constant appears nowhere: no expression in the tree subtracts one partition
base from another, no shift by 28 exists outside `1u << 28`, and `arch_context_init` cannot reach
it, `boot_stack_alloc` asserting the block inside this node's own arena. It is simply whatever the
other core last left in `g_arch_next->sp`, so it is a CONSEQUENCE and not a clue: a later reader
should not go hunting for arithmetic that produces it. `0xf000_0000` is unmapped on this part,
which is why the read faults with `BFARVALID` rather than returning rubbish.

**THE SHARED REGION IS THE WAY TO READ A PEER, AND THE DEBUG WINDOW IS NOT.** Three sessions were
spent trying to read a running peer from outside and none produced a believable value; a peer
writing four words into the region both nodes already share worked on the first attempt.
`CONFIG_KICKOS_AMP_DIAG_REPORT` is that instrument, off by default. Three of its five cells carry
values the primary ALREADY KNOWS, and that is the whole design: with a peer that provably cannot
write -- a three-instruction assembly park -- it reads `cell0=0x0`, and with a live peer it reads
back the node index, the build's own tag and the core clock this node derives its timing from. A
plausible reading is the trap here, not an implausible one, and an instrument with no known-value
cells cannot tell you which it gave you.


**THE TWO 4 KiB NON-STRIPED SRAM BANKS ARE A CONTENTION WIN AND NOT SPARE MEMORY.** SRAM is 520
KiB in ten banks: 512 KiB striped as two 256 KiB regions, word-striped four ways on address bits
3:2, plus `SRAM8` at `0x2008_0000` and `SRAM9` at `0x2008_1000` outside the striped range (Tables
11 and 12, section 2.2.3; section 4.2). Each has its own AHB5 arbiter and accesses to different
banks proceed simultaneously (section 2.1.1), so a bank per node is a core its peer cannot stall.
The datasheet names that use itself, twice: "useful for hoisting high-bandwidth data structures
like the processor stacks" (2.2.3) and "for per-core purposes (e.g. stack and frequently-executed
code), guaranteeing that the processors never stall on these accesses" (4.2). The AMP partition
therefore divides the striped 512 KiB ONLY and leaves both banks unclaimed. The emulated vehicle
has no analogue, so claiming them as ordinary partition memory would have spent the win
invisibly.

**THE SIO FIFO DEPTH IS STATED TWICE AND THE TWO DISAGREE.** Section 3.1.5 and Figure 7 say each
inter-processor FIFO is four entries deep; the `FIFO_ST` register description in section 3.1.11
says eight words. Nothing in the tree sizes anything on it, and the core-1 launch handshake is
push-one/echo-one so it is depth-independent by construction. **A later user of that FIFO may not
be**, which is the only reason this is written down: measure it rather than taking either figure.

**AND A READING RULE ABOUT GATE REGISTRATION, paid for twice in one milestone.** A gate keyed on
the wrong predicate fails in whichever way its predicate happens to fail, and the two ways look
nothing alike. `console_reach` registers only for a preset with a row in its declaration file and
is SILENT without one, so adding a preset drops a gate its siblings run and nothing says so. The
AMP vehicle gates were registered on any own-image board while `kickos_add_qemu_test` refuses a
board it does not know, so a board with no emulator did not merely lack gates, it FAILED TO
CONFIGURE. Same defect class, opposite symptoms, and the only difference was which predicate was
wrong. **A reader who fixes one should look at the other**, and the check is per-gate rather than
per-declaration-file: `trap_redzone` owes nothing on a board with no rows, and that is not
symmetry to assume but a thing to verify per gate.
