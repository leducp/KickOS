<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# What a chip adds to its core

> Part 4 followed the ISA: registers, trap entry, context switch, the interrupt
> controller a core brings with it. This part follows what a vendor builds around
> that core to make a chip: clocks, pins, serial ports, memories, protection hardware,
> a boot ROM, and the quirks of real silicon. Two chips with the same core share every
> line of kernel code and still differ in all of that. The chapter teaches what those
> differences are, how an OS keeps them out of its kernel, and how KickOS splits ISA,
> chip and board. Each board's exact facts live in `../reference/boards.md`; the code
> that brings the f411 example's chip up is walked in `../journey/01-power-on.md`.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1 (section 1.3, the
> hardware review, which ends with booting the computer), ch.5 (I/O hardware: device
> controllers, and the device-dependent layer of the I/O software), and ch.12
> (operating system design: hiding the hardware).*

## Same core, different machine

Four chips KickOS runs on carry the same core, an ARM Cortex-M4: the STM32F411 of
the f411 example, the STM32F302, the Infineon XMC4800 and the NXP K64F. They execute
the same instructions, take interrupts through the same NVIC, have the same SysTick
timer, and switch threads with the same assembly (`arch/arm/armv7m/switch.S`). Part 4
has nothing more to say about any of them.

Yet an image built for one does not run on another. Flash and SRAM sit at different
addresses, the console is a different UART on a different bus, its interrupt line has
a different number. Even the MPU differs, because it is an *option* of the Cortex-M4
that the vendor may leave out: the STM32F302x8 has none, and the K64F puts its own
unit, the SYSMPU, on the bus instead.

The ISA is the contract between the compiler and the core. The **chip** (or SoC) is
everything the vendor adds around that core: the interconnect, the memories, the
peripherals, the clock and reset logic, and the ROM that runs first. The **board** is
what the board designer adds around the chip: the crystal, which pins reach which
connector, the LED, external flash. A kernel that wants one design across many parts
has to know which of these three each fact belongs to.

## What a chip adds, one layer at a time

### Clock trees and PLLs

At reset a microcontroller runs from an internal RC oscillator: always present, but
too imprecise for a reliable serial line. A faster, precise clock is built at run time
from a **crystal** on the board, multiplied by a **PLL** (phase-locked loop) and
divided by **prescalers** for each bus. This **clock tree** feeds the core, each bus
and each peripheral, so a UART's baud divisor or a timer's tick length is only correct
for one setting of it.

Bringing the tree up has an order, and the order is chip-specific. The f411 example:

- At reset the core runs on the 16 MHz internal oscillator (HSI).
- The flash needs more wait states before the core runs faster, or the first
  over-speed instruction fetch faults. So the wait states are widened *first*.
- The board's 8 MHz crystal (HSE) is started. The PLL divides it to 1 MHz,
  multiplies by 336 and divides by 4: an 84 MHz system clock.
- The bus prescalers are set before the switch, so the slow peripheral bus (APB1)
  never briefly exceeds its 42 MHz limit: it runs at 84 / 2 = 42 MHz.
- Only then does the system clock switch to the PLL.

Two more chip facts hide in that tree. When the APB1 prescaler divides, STM32 parts
double the timer clock, so TIM2 counts at 84 MHz, not 42. And every peripheral has a
**clock gate** in the clock controller (RCC) that must be set before its registers
answer; on this chip an access to a gated peripheral is a bus fault. The RP2040 builds
its tree differently: a 12 MHz crystal, a PLL to 125 MHz, and a 1 MHz timer tick from
a divider in its watchdog block.

### Pin muxing

A chip has far more peripheral functions than pins. Each pin therefore carries a
**multiplexer**: a field in a configuration register selects whether the pin is a
plain GPIO or one of several **alternate functions**. On the STM32F411, pins PA2 and
PA3 become USART2's transmit and receive lines when their alternate-function field
selects function 7. Other chips draw this differently: the RP2040 has a per-pin
function select; the ESP32-C6 has a **GPIO matrix** that can route almost any
peripheral signal to almost any pin. Chapter 8.4 shows why the mux matters to
isolation: on some chips the register that toggles a pin is separate from the one
that muxes it, on others it is not.

### The console UART

Every chip has serial ports; which one is the console is partly the chip's choice
and partly the board's (which UART's pins reach a USB bridge or a debug probe). The
chip decides the rest: the register layout, which bus clock feeds it, and how the
baud divisor is computed from that clock. On the f411 example the console is USART2,
on APB1, so its divisor is computed from 42 MHz for 115200 baud. That is why the
clock tree must be final before the console is programmed. On the ESP32-C6, the boot
ROM has already programmed the console UART before the first KickOS instruction, and
KickOS keeps the ROM's divisor rather than recomputing it.

### The memory map and the interrupt lines

Part 0.1 showed how to read a memory map. What changes per chip is every number in
it: where flash starts (`0x08000000` on the F411, an external QSPI flash behind an
execute-in-place window at `0x10000000` on the RP2040), where SRAM starts and how big
it is (`0x20000000`, 128 KiB on the F411; `0x40800000`, 512 KiB on the ESP32-C6), and
where each peripheral sits. The interrupt controller belongs to the core, but the
**line numbers** belong to the chip: the F411's NVIC has 86 device lines, USART2 is
line 38 and TIM2 is line 28. On the ESP32-C6 a peripheral does not even own a fixed
line: an interrupt matrix routes each source onto one of the CPU's interrupts, and
software programs that route.

### Where the protection hardware sits

Chapter 3.7 develops this; the short form belongs here because it is a chip fact.
A unit on the **CPU side** sees every access the core makes, so it can confine a
thread to its own device registers: the F411's PMSAv7 MPU. A unit on the
**bus-slave side** sees only the traffic to what it guards: the K64F's SYSMPU never
sees the peripheral bridge, so the bridge gates devices per 4 KiB slot and per
privilege, and an open slot is open to every unprivileged thread. The ESP32-C6 has
both in series: a CPU-side PMP that faults per thread, and a bus-side APM set once per
security mode that, on a denial, reads zero or drops the write instead of trapping.
One kernel promise, "this thread may touch this device and nothing else", is
enforced on the first chip, unenforceable per thread on the second, and split
across two units on the third.

### Reset and the boot ROM

A Cortex-M reads its first stack pointer and reset address from the vector table;
*where* that table is, and what runs before it, is the chip's. On the F411 the flash
at `0x08000000` is aliased to address 0 at boot, so the core reads KickOS's table
directly. On the RP2040 a boot ROM copies the first 256 bytes of the external flash
into SRAM, checks their CRC and jumps there; that second-stage loader
(`arch/arm/chip/rp2040/boot2.S`) sets up execute-in-place before KickOS's reset
handler can run from flash at all. On the ESP32-C6 the ROM copies the image into SRAM
and starts it there. Chips also differ in what they leave armed: the ESP32-C6's ROM
arms watchdogs that reset the part within seconds, and the K64F's watchdog resets it
about 238 ms after reset unless disabled first. A boot ROM is often also the way back
into firmware download mode (Chapter 8.6).

### Errata and quirks

A chip and its manual disagree. Vendors publish **errata sheets** for known silicon
bugs, and porters collect the rest. In the F411's chip code: the core's cycle counter
(DWT) is not the clock, because on parts in this fleet it can intermittently read
garbage, which a wrap-extending clock turns into a jump of 2^32 ticks (Chapter 2.1);
and TIM2's low-power clock enable must stay set, or the clock stops when the idle
thread executes `WFI`. On the K64F, the two watchdog unlock keys must land within 20
bus cycles of each other, so the pair is one assembly block no optimisation level can
split. Such a workaround is invisible unless a comment says why, which is one of the
few comments KickOS keeps.

## How an OS can organise all this

The design space has four main shapes, and real systems mix them.

- **Link the vendor's library.** The vendor's HAL or SDK brings up the clocks and
  drives the UART: the fastest first boot. The cost is a large, separately
  maintained body of code per vendor inside the kernel, with its own allocation and
  interrupt assumptions and its own idea of who owns the hardware.
- **Conditional code.** One source with `#ifdef CHIP_X` blocks. Fine for two chips;
  with a dozen, no reader can see what a build compiles, and a new chip edits files
  every other chip depends on.
- **A hardware description, separate from the code.** Addresses, sizes, interrupt
  numbers and wiring live in a data file, and the code uses names. Linux reads a
  device tree passed in at boot, so one binary runs on many boards, at the cost of a
  parser and late failures; Zephyr compiles its devicetree into headers at build
  time, which fails early and costs nothing at boot, at the price of one image per
  board.
- **Layered directories.** ISA, chip and board code live apart behind a defined
  seam, so a new chip with a known core touches only the chip layer.

## What KickOS chose, and why

KickOS takes the last two together, and writes its chip code itself rather than
linking a vendor library.

**Three layers.** The ISA layer is `arch/<family>/<isa>/` (for the f411 example,
`arch/arm/armv7m/`), and Part 4 is its tour. The chip layer is code in
`arch/<family>/chip/<chip>/` (`arch/arm/chip/stm32f411/`) and data in
`platform/<chip>/chip.yaml`. The board layer is data in
`platform/<chip>/<board>.yaml` (the console device and its pins, the LED the kernel
owns, buses and parts) and the build descriptor, configurations and default
composition in `boards/<board>/`. The kernel above all three calls one seam,
`arch/include/kickos/arch/arch.h`, and a chip answers the part of it that is its own:
`arch_init`, the console, the clock read, the pin mux, the diagnostic LED. Two chips
of one vendor family can share a body (`arch/arm/chip/stm32f1f3/` for the STM32F103
and F302, `arch/arm/chip/rp2xxx/` for the RP2040 and RP2350) without either becoming
an `#ifdef` of the other.

**The chip file is the truth for the memory map and the resources.** Every address,
size, line count, device window and pin function in `platform/stm32f411/chip.yaml` is
written once, with the manual page it comes from. At configure a host tool reads it
and writes the headers the chip code compiles against (`kickos/chip_mmap.h` for the
device bases, `kickos/chip_limits.h` for the line count, `irq.h` and `chip_layout.h`
for the line numbers and the linker's memory regions), and the same tool admits a
composition against it: which device windows a task may be granted, and which
devices are `owner: kernel` and so granted to no one (the F411's TIM2, clock
controller and GPIO ports). The F411's vector table is sized by the generated
`KICKOS_MAX_IRQ`, so it and the kernel's interrupt table cannot disagree. A base
address restated in a C header, a linker script and an admission table eventually
would.

**The chip code names no board pin.** The board file says the console is
`/dev/usart2` on PA2 and PA3 and the LED is PD12; configure turns that into
`board_pins.h`, and the chip code muxes those pins, `static_assert`s that the
console is the UART it drives, and refuses those pins in `arch_pinmux_set`. So the
F411 Discovery and the Black Pill share one chip backend and differ only in their
board layers: the board file, and the one wiring fact it has no field for, the
crystal frequency, in `boards/<board>/include/kickos/board_wiring.h`
(`KICKOS_HSE_HZ`).

**Build time, not run time.** A KickOS image is built for one board. Nothing parses
a description at boot, and a wrong fact stops the build instead of the running
system: a chip with no chip file, or a board with no board file, is refused at
configure, and a board console that is not the UART the chip code drives fails the
compile.

**Clean-room chip code, no vendor library.** Each backend is written from the
reference manual, register by register. It costs porting effort, and it buys a chip
layer that follows the kernel's rules (the seam, the boot order, who owns which
device) rather than a vendor's, and that a reader can follow in one file.

## The f411 example through the three layers

`arch_init` in `arch/arm/chip/stm32f411/chip_stm32f411.cc` is the chip's one
bring-up function, and its order is the clock-tree argument above made code:

1. `clock_init`: flash wait states, the crystal, the PLL, the bus prescalers, then
   the switch to 84 MHz. If the crystal never starts, it stays on the 16 MHz HSI and
   the console still works at a divisor computed from that clock.
2. `tim2_clock_init`: the clock gate for TIM2, then the 32-bit counter the kernel
   reads as its time base, with its wrap interrupt on line 28.
3. `usart2_init`: the clock gates for the console's GPIO ports and for USART2, the
   pin mux from `board_pins.h`, then the baud divisor from the final APB1 clock.
4. `kickos_armv7m_init`: the part every ARMv7-M chip shares, back in the ISA layer.

Each fact it uses has one home:

| Fact | Layer | Where |
|---|---|---|
| The context switch and the trap entry | ISA | `arch/arm/armv7m/` |
| Which core and FPU (Cortex-M4F) | chip, build | `arch/arm/chip/stm32f411/cpu.cmake` |
| USART2 at `0x40004400`, TIM2 at `0x40000000`, lines 38 and 28, 86 lines, flash and SRAM | chip, data | `platform/stm32f411/chip.yaml` |
| PLL settings, wait states, baud formula, the TIM2 quirks | chip, code | `arch/arm/chip/stm32f411/chip_stm32f411.cc` |
| Console on USART2 at PA2/PA3, the kernel LED on PD12 | board, data | `platform/stm32f411/f411disco.yaml` |
| The 8 MHz crystal | board, header | `boards/f411disco/include/kickos/board_wiring.h` |
| Arch, chip, configurations, default composition | board, build | `boards/f411disco/` |

## Where to go next

- `../reference/boards.md` holds each board's facts: the status matrix (core,
  console, LED, flash tool), the per-board caveats, and the hardware facts a porter
  cannot derive from a manual.
- `../reference/porting.md` holds the contract for a new chip or board ("Adding a
  board/chip (the seven edit points)", "Chip-family units", the pin-mux and
  peripheral-enable seams, "Clock retune"), and Part 6 is the porting guide over it.
- `../journey/01-power-on.md` shows the code: Stop 1 (the vector table and the flash
  alias), Stop 4 (`arch_init` and the clock tree), Stop 5 (the early console on
  USART2) and Stop 11 (TIM2 as the clock), each with its f411 note.
- Chapter 0.1, *[The machine underneath](the-machine-underneath.md)*, teaches how to
  read the memory map and register tables these files encode.
- Chapter 2.1, *[The time base](the-time-base-a-monotonic-clock-you-can-trust.md)*,
  is the porter's checklist for the clock each chip must provide.
- Chapter 3.7, *[Peripheral isolation and the hardware
  ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*, follows CPU-side and
  bus-side protection across the fleet.
- Chapter 7.6, *[The CPU reads ahead](memory-types-and-speculative-access.md)*, is
  the memory-map quirk that stalls a core without a fault.
- Chapter 8.4, *[The fast path is the
  capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*, and Chapter
  8.6, *[Rebooting into the bootloader](rebooting-into-the-bootloader-and-who-may-ask.md)*,
  take the pin mux and the boot ROM further.
