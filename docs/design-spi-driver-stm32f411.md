<!-- SPDX-License-Identifier: CECILL-C -->
# Design brief: STM32F411 SPI1 loopback driver -- the canonical PMSA per-thread MMIO isolation reference

Design + build-only (F411 not on the bench; K64F is). The CANONICAL per-thread
peripheral-MMIO isolation reference on ARMv7-M PMSA -- the result the fleet was missing.
Counterpart of `design-spi-driver-k64f-dspi.md` (coarse AIPS ceiling) and the XMC/USIC
brief. Builds on the landed MMIO-grant seam (`design-task9-mmio-driver.md`;
`kos_thread_params.mmio_base/mmio_size`), the tier-1 IRQ path (`kos_irq_register/wait/
ack`), and the k64drv privileged-shim -> unprivileged-driver structure
(`user/apps/k64drv/main.cc`).

## Why this exists -- the honest gap it closes (read first)
On PMSA the MPU is CPU-side and covers ALL address space, peripherals included. A granted
DEV MMIO window is therefore a GENUINE per-thread capability: the per-thread MPU is reprogrammed
on every switch-in (`arch_mpu_apply` stashes the grant, `kickos_arch_mpu_commit` programs it from
the switch epilogue), so a thread reaches exactly its granted registers and an
ungranted peripheral access faults MemManage. This is structurally unlike K64F, where the
SYSMPU is bus-slave-side (flash/SRAM/FlexBus only) and peripherals are gated by the AIPS
bridge per-4KB-slot, all-user -- k64drv proved a K64F peripheral window grant is INERT and
the ungranted poke does NOT fault (see the k64drv brief's "hardware ceiling").

The XMC explorer flagged the fleet's one honest gap: **a real peripheral window granted to
an unprivileged thread has never been run on PMSA silicon** -- the selftest `mmio_grant`
only asserts the two boundary REJECTIONS, and the "MMIO" region it exercises is arena RAM.
PMSA peripheral enforcement itself is, to date, only build/link-validated. This driver
first-proves it on F411 silicon: prove granted-SPI-works AND ungranted-peripheral-faults,
per-thread.

## Confirmed hardware facts (RM0383 Rev.-, STM32F411xC/E; line refs = `pdftotext -layout`)
| Fact | Value | RM0383 citation |
|---|---|---|
| SPI1 base | `0x4001_3000` (block `0x4001_3000..0x4001_33FF`, SPI1/I2S1) | memory map (line 1753) |
| SPI1 pins | AF5, `PA4/PA5/PA6/PA7` | memory map (line 1898); AF table AF5 (SPI1..4) (lines 7604/7629) |
| Clock enable | `RCC_APB2ENR` (off `0x44` from RCC base `0x4002_3800` = `0x4002_3844`), `SPI1EN` = **bit 12** | 6.3.11 APB2ENR off 0x44 (line 6083); "Bit 12 SPI1EN" (line 6130) |
| NVIC IRQ | **35** (vector 42, `0x0000_00CC`); single SPI1 vector, all sources | vector table (line 10857) |
| GPIOB base (ungranted negative-test target) | `0x4002_0400` | memory map (line 1731) |

Pin assignment (AF5, master, SOFTWARE NSS so PA4/NSS is NOT muxed): **PA5=SCK, PA6=MISO,
PA7=MOSI** (the canonical STM32F4 SPI1 AF5 map; loopback jumpers PA7->PA6).

SPI register offsets from `0x4001_3000` (RM0383 20.5, TOC lines 731-734; standard STM32 SPI
map, offsets confirmed against 20.5.3/20.5.4 headers at lines "Address offset: 0x08 / 0x0C"):
| Reg | Off | Fields used (RM0383 20.5 line refs) |
|---|---|---|
| `CR1` | `0x00` | MSTR b2 (line 33109), BR[2:0] b5:3 (33098), CPOL b1 (33120), CPHA b0 (33128), SSM b9 (33079), SSI b8 (33084), SPE b6 (33093), DFF b11 (33059, 0=8-bit), LSBFIRST b7 (33088, 0=MSB) |
| `CR2` | `0x04` | RXNEIE b6 (33150), TXEIE b7 (33147) |
| `SR`  | `0x08` | RXNE b0 (33246), TXE b1 (33243), BSY b7 (33205) |
| `DR`  | `0x0C` | data[15:0]; write=TX, read=RX; reading DR clears RXNE |

**Granted window: base `0x4001_3000`, size `0x20` (32 B).** Minimal PMSA-encodable window
covering CR1/CR2/SR/DR (`0x00..0x0C`; the 32 B span reaches `0x1C`). `arch_mpu_region_
encodable(0x4001_3000, 32)`: PMSA min = 32, size 32 is pow2 >= 32, and `0x4001_3000 & 31 ==
0` -> naturally aligned -> **encodable**. This is the minimal encodable window containing
CR1/CR2/SR/DR; the 32 B min forces CRCPR/RXCRCR/TXCRCR/I2SCFGR (`0x10..0x1C`) to ride along.
That is self-DoS-only (the driver could flip SPI1 into I2S mode) -- no cross-peripheral
over-grant, no escalation (`arch_arm_common.cc` mpu_rasr -> AP_RW | XN | MEM_DEVICE).

## Privilege split (mirror kickos_xmc_usic_init / k64drv; the shim `main` runs privileged)
The unsafe one-time setup the unprivileged driver must NOT be able to do -- CLOCK-ENABLE
and PIN-MUX -- stays privileged and OUT of the granted window (RCC `0x4002_3800`, GPIOA
`0x4002_0000`). Those are the escalation surfaces; keeping them out of the 32 B SPI1 window
is what makes the window a real capability.

Privileged bring-up shim (once):
1. `RCC_AHB1ENR |= GPIOAEN` (clock GPIOA), `RCC_APB2ENR |= SPI1EN` (bit 12).
2. GPIOA mux: PA5/6/7 MODER=AF(0b10), OSPEEDR=high, AFRL nibble = 5 (AF5). PA4/NSS left
   as-is (software NSS). Muxing SCK before CR1 is glitch-free ONLY because CPOL=0 is the
   CR1 reset value (SCK idle level already correct); a CPOL=1 variant must write CR1 first.
3. SPI1 config while SPE=0: `CR1 = MSTR | SSM | SSI | BR(/64) | CPOL0 CPHA0 | DFF0 LSBFIRST0`,
   then set SPE. SSM=1 + SSI=1 hold the internal NSS high so the master is not deselected
   (else MODF). CR2 left 0 (RXNEIE off; the driver arms it in-window).
4. Spawn the UNPRIVILEGED driver: `mmio_base=0x4001_3000, mmio_size=0x20`, privileged=false.
   The driver registers SPI1 IRQ 35 via the tier-1 path. Everything after boot is in-window
   MMIO + IRQ wait.

## Transfer model: IRQ-driven (tier-1, reused unchanged)
`int spi_transfer(void* tx, void* rx, size_t len)` -- blocking, single-word full-duplex
(RXNE-complete implies TX shifted out). Chosen over a poll because it exercises the second
half of the isolation claim -- an unprivileged thread OWNING a granted device IRQ line --
and reuses the k64drv/tier-1 mechanism verbatim (register/wait). The RXNE-clean-before-
the-next-wait step is the SPI analogue of k64drv's TFLG W1C:

Per word: bounded-poll `SR.TXE` (in-window) -> write `DR`=tx (starts the exchange) ->
`kos_irq_wait` on line 35 -> read `DR`=rx (this CLEARS RXNE and de-asserts the line, so it
does not storm on unmask -- the mandatory quiesce, still required BEFORE the re-arm; SPI has
no W1C flag) -> `kos_irq_ack` (OPTIONAL: the loop's next `kos_irq_wait` auto-re-arms line 35;
keep the explicit ack only for the compute-then-wait shape).
Only RXNEIE is armed (CR2 b6), so although line 35 is a single vector for all SPI1 sources,
only RXNE wakes it (mirror of the K64F EOQF-only pattern). RXNEIE is armed once in-window
before the loop.

## Loopback + the negative test (the canonical proof)
STM32 SPI has no internal loopback bit -- the test uses a physical **PA7 (MOSI) -> PA6
(MISO) jumper**, so a transmitted byte reads back equal. The app writes a known pattern
(0xA5, 0x3C, 0x00, 0xFF) and prints PASS/FAIL per word (`rx == tx`).

Then the driver announces-before-poke (k64drv idiom) and reads an UNGRANTED peripheral --
GPIOB `0x4002_0400`, outside the 32 B SPI1 window. On PMSA this MUST fault MemManage;
`kickos_armv7m_fault_report` labels it "MPU FAULT" and prints `MMFAR=0x40020400` (a data
access violation sets MMFSR/MMARVALID; F411 startup.S routes MemManage -> HardFault_Handler
-> the reporter; MEMFAULTENA is set by `kickos_arm_mpu_program`). This is the per-thread peripheral
isolation result the fleet was missing -- and, being terminal, it is the LAST thing the
driver does.

## Region / slot budget
Unprivileged driver: app code (RX) + app static-data (RW-NX) + private stack + SPI1 MMIO
(32 B) = **4 of 8** (HW-free software NSS, no GPIO/CS region, no domain-data grant). The
`thread_create` budget assert covers it.

## Open questions / risks
1. **PMSA peripheral enforcement is first-PROVEN here** -- to date only build/link-validated
   on F411. If the ungranted GPIOB poke does NOT fault, PMSA does not cover peripheral space
   as modeled and the whole per-thread-capability story needs revisiting (it should: the MPU
   background/PRIVDEFENA map only exempts PRIVILEGED code, and the driver is unprivileged).
2. **F411E-DISCO shares PA5/6/7 with the onboard gyro (L3GD20 / I3G4250D) on SPI1; its SDO
   drives PA6/MISO.** Confirmed against UM1842 pin table: PA5=SPI1_SCK, PA6=SPI1_MISO(+gyro
   SDO), PA7=SPI1_MOSI, and the gyro CS = **PE3** ("CS_I2C/SPI"). The shim therefore drives
   PE3 HIGH (GPIOE output) before any SCK activity so the gyro stays deselected / SDO
   tri-stated and the PA7->PA6 jumper owns MISO. If a scope still shows the gyro fighting
   MISO, the fallback is **SPI2 on PB13/14/15** (no onboard SPI device) -- but that changes
   the base (SPI2 `0x4000_3800`), the clock (APB1 not APB2), and the pins/AF; verify against
   the schematic before switching. Console stays USART2 either way. (A Black Pill has nothing
   on PA5/6/7 -- cleaner -- but the board target is the Disco.)
3. **Fault decode:** an ungranted MMIO access on PMSA is a clean MemManage (not the K64F
   BusFault path); the reporter's MMFAR print is the oracle. Confirmed by construction.
4. **Baud is not critical** for a short jumper (no real device timing); /64 (~1.3 MHz on the
   84 MHz APB2) is a conservative default and default GPIO high-speed covers it.

## Sequencing
Pure addition, no kernel change: transfer path = in-window MMIO + the existing tier-1 IRQ
event path; the only device-specific step is the RXNE quiesce before the next wait. Order: (1)
privileged shim (clock/mux/config), (2) unprivileged driver + `spi_transfer`, (3) loopback
words, (4) the ungranted-poke MemManage proof. Build with `-DKICKOS_HAVE_MPU=1` (the app
`#error`s otherwise -- it exists to prove enforcement). Build-only; the operator swaps in the
F411, wires PA7->PA6, and validates.

Bench: the 32F411E-DISCO ST-LINK/V2 is SWD-only -- **no VCP** (UM1842; `reference/boards.md`).
So it is one USB to FLASH (onboard ST-Link) but the CONSOLE needs an external FTDI on PA2
(USART2_TX), GND-GND, 115200 8N1 -- not the ST-Link USB.
