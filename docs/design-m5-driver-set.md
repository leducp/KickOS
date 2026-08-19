<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design: what "complete the driver set" actually owes

> **Status: SURVEYED FROM THE TREE, scope not yet approved.** The matrix below is derived from
> `boards/*/board.cmake`, `system/driver/CMakeLists.txt`, the `arch/*/chip/*/chip_*.cc` strong
> symbols and the `arch/common/*_default.cc` null seams, not from `roadmap.md`, `TODO.md` or
> `docs/reference/boards.md`, which were consulted only to catch a board the build system hides.
> They hide none: 20 `board.cmake` files, 20 documented boards.

The milestone says "complete the driver set". That phrase has no meaning until the set is
enumerated, and enumerating it changed what the work is. Three gaps are mechanical and one is
architecture.

## 1. Three states, and why the distinction is the whole survey

A cell in the matrix is one of:

- **REAL**: a strong per-chip symbol doing register I/O.
- **STUB**: the weak default links unmodified. The tree uses null objects deliberately, so a stub
  is a *working build with no driver*, which is exactly the thing that reads as "present" in a
  build log and as "absent" on a scope.
- **absent**: no seam of that kind exists at all.

An inventory that does not separate STUB from REAL will report this fleet as far more complete than
it is. That is the trap this page exists to avoid.

## 2. Where the driver set actually stands

Counting by CHIP (16 of them, since `mps2` serves four boards and `stm32f411` two), not by board.

| class | REAL on | STUB or absent on |
|---|---|---|
| console (arch, polled) | **16 of 16** | none; the one class with no gap, and it has no fallback TU by design |
| timer/clock (arch) | **16 of 16** | none |
| pinmux | 12 | `mps2`, `nrf51`, `virt`, `sim` |
| diag LED | 11 | `mps2`, `nrf51`, `rp2350`, `virt`, `sim` |
| UART as a SERVICE | 8 chips have one | `stm32f103`, `stm32f302`, `sam3x8e`, `nrf51`, `mps2`, `virt` have no directory at all |
| USB (console carrier only) | `imxrt1062`, `rp2040`, `rp2350` | not a generic peripheral class anywhere |
| SPI as a registered service | **2**: `mk64f`, `xmc4800` | everywhere else |
| GPIO | **4**: `mk64f`, `xmc4800`, `esp32c6`, `rx72m`, by THREE unrelated mechanisms | 12 chips |
| reboot / bootloader handover | **3**: `rp2040`, `rp2350`, `imxrt1062` | 13 chips return `-KOS_ENOSYS` |
| **I2C** | **0** | **16 of 16** |

Two rows deserve their own reading rather than a count.

## 3. The finding that matters most: a contract with one consumer, and it is simulated

**`KOS_SVC_UART`, the general bidirectional UART PORT service kind, has exactly one consumer in
the entire tree, and it is a host loopback.** `system/init/sim/service_list_uart.cc` is the only
file that assigns it; its own comment says "This list deliberately publishes NO console. It is a
`KOS_SVC_UART` port", and its device is host fd 1 with TX fed back into RX.

Every UART driver on real silicon (`mk64f`, `xmc4800`, `stm32f411`, `rx72m`, `esp32c6`, `esp32`)
runs as `KOS_SVC_CONSOLE` instead. They share the `<kickos/driver/uart.h>` register backend and the
two-thread `irq_loop`/`serve_loop` framework, so THAT much is silicon-judged. What is not judged is
the port contract itself: back-pressure with a real producer, baud mismatch, and overrun on a
controller that can actually overrun.

`design-m4.6-irq-driver.md` records this as a strength, "all CI-gated against a real driver", and
against the loopback that is true. What the row does not support is the reading that matters:
**a gate over a cooperative simulated device does not judge a contract whose hard cases are all
uncooperative hardware.** This is the same class of finding the KickCAT reality check produced, and
it is cheap to fix: promote ONE existing silicon UART to a real `KOS_SVC_UART` port.

## 4. I2C: the empty row is a missing driver, not a missing seam

The WIRE ABI in `user/include/kickos/sys/bus.h` was authored for both protocols from the start, and
it already specifies I2C in detail:

- `enum kos_bus_proto { KOS_BUS_SPI = 0, KOS_BUS_I2C = 1 }`: the discriminator exists.
- `KOS_BUS_SEG_RD`: per-segment direction, because I2C is half duplex where SPI is not.
- `KOS_BUS_SEG_STOP`: "STOP after this segment (absent = repeated START)". **That is the
  repeated-START idiom, expressible today.**
- `KOS_BUS_ADDR_10BIT` in `kos_bus_mode`, deliberately overlapping SPI's `CPOL` bit because the
  protos are distinct.
- `kos_bus_req.device` is documented as "CS index / I2C address slot"; `kos_bus_rsp` documents the
  reply payload for both ("I2C: rd-segment data concatenated"); the request payload rule likewise
  ("I2C: wr-segment bytes only, rd segments carry no request bytes").

So a register read on the FXOS8700CQ, meaning write the register address, repeated START, read N
bytes, is **two segments in ONE request**: segment 0 writes with no STOP flag, segment 1 reads with
STOP. One transaction, one IPC call, no cross-call state hold. That is the same property the KickCAT
reality check needed from SPI, and the ABI already grants it.

What is genuinely missing is smaller than "a seam": an i2c class header beside `spi.h` in the same
driver include directory, mirroring its four-symbol shape; a per-chip engine; the proxy that
marshals onto a service endpoint; and a service driver. **That is porting work against a designed
ABI**, not architecture. `KOS_SVC_I2C` being unassigned means no DRIVER has been written, not that
nothing was thought through.

Several chips already driven here have an I2C-capable peripheral beside the block the tree already
drives (`xmc4800`'s USIC and `rx72m`'s SCI are multi-protocol), so the silicon is not the obstacle.

**And it lands in the same trap section 3 just described**, which is the argument for how to do it:
whichever chip goes first judges the API alone. **Do two chips on two different ISAs, or the seam
ships unjudged.** The SPI seam has two implementations and the KickCAT port then found a real
mismatch in it; a one-implementation seam would not have survived that contact.

## 5. GPIO has three mechanisms and no contract

Real GPIO exists on four chips by three unrelated routes: a per-chip `#if` ladder inside a user app
(`user/apps/common/gpioblink/CMakeLists.txt` compiles register code only for `xmc4800` and `mk64f`,
and every other chip hits an explicit "no GPIO register layout for this board; parking" branch), and
two bespoke capability-leaf classes (`esp32c6`'s `gpio_class`, `rx72m`'s `port_class`) each consumed
by that chip's own isolation-proof app.

None of these is an arch-level contract the way `arch_pinmux_set` and `arch_diag_led_set` are. So
extending GPIO to a fifth chip touches one file, but doing it without adding a FOURTH mechanism is
seam design. **The refusal to write here is the easy one**: do not add a fifth chip to the ladder.
Either unify first or leave it alone, because a fourth mechanism is a second truth by the
tiebreaker's own terms.

## 6. What is genuinely mechanical

These need no new contract and no design pass. They plug into a seam that already generalises across
several chips.

- **Reboot on `esp32c6`, `esp32`, `rx72m`.** `arch_reboot` is an existing arch contract with a
  working `-KOS_ENOSYS` default and three implementations. These three are the most
  driver-equipped chips in the fleet and each still costs a button press per bench capture, so this
  one pays for itself in the bench loop immediately.
- **UART service on `stm32f103`, `stm32f302`, `sam3x8e`, `nrf51`.** The `system/driver` chain and
  the `<kickos/driver/uart.h>` backend already generalise across eight chips; a ninth is mechanical.
- **SPI as a service on `stm32f411`.** The register code EXISTS and is proven on silicon; it just
  lives inside a one-off diagnostic app (`user/apps/f411disco/f411spi/`), registered for
  `f411disco` only, so `blackpill` cannot use it despite sharing the chip. Turning it into a
  `KOS_SVC_SPI` service following `mk64f` and `xmc4800` is a move, not an invention, and it makes
  the SPI seam's implementation count three.

Note also that a UART service EXISTING is not the same as a board USING it: only `frdmk64f` and
`xmc4800-relax` set `KICKOS_SERVICE_LIST` in their Kconfig at all. Every other board defaults to
`kickos_services_none`, with the repeated comment "no silicon witness yet". That is a
default-selection gap, not a driver gap, and it should not be counted as missing work.

## 7. The scope question, stated for a decision rather than assumed

Section 5 is architecture. Sections 4 and 6 are porting, section 4 against an ABI that is already
designed. The milestone-scope rule says no implementation outside the current milestone unless it is
small and orthogonal, and that cuts cleanly here:

- Section 6 is unambiguously in: mechanical, orthogonal, and the reboot item pays back inside this
  milestone's own bench loop.
- Section 3 is in and cheap: one existing driver, promoted, so a contract stops being judged only
  by a loopback.
- **I2C is IN.** The wire ABI is designed (section 4); what is owed is a class header, three engines
  and a service. The first consumer is the FXOS8700CQ accelerometer/magnetometer on the FRDM-K64F,
  which is what stops the API being judged by its own author: a real device answers or it does not.
- GPIO unification is NOT proposed for M5. Nothing forces it, and the cheap discipline is simply to
  refuse a fourth mechanism.
