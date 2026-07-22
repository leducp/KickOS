<!-- SPDX-License-Identifier: CECILL-C -->
# M4 driver-coverage matrix -- per-board peripheral survey + complexity/gain backlog

**EXPLORATORY -- NOT A CONTRACT.** The complement of `docs/design-driver-era-scope.md`
(the WORK) and `roadmap.md` (the milestone): this doc is the SURVEY -- what each of the
four neutrality boards actually carries on silicon, what KickOS already drives, and a
complexity-vs-gain weighting that turns "full per-board coverage" from an unbounded wish
into a PRIORITIZED backlog.

Framing (maintainer's revised scope): the ASPIRATION is FULL per-board peripheral coverage,
NOT a set bounded a-priori to the service-defined classes (console/UART, gpio, pinmux,
clock/power, SPI/I2C). Reaching for a board's *whole* peripheral complement is exactly how
we discover hardware that needs a dedicated or NEW service API -- that discovery IS the
point. The bound is not a class restriction; it is the weighting below. The weighting -- not
an a-priori list -- is what stops scope ballooning into a four-vendor BSP.

---

## 0. Outline
1. Legend -- coverage marks, the two weighting axes, the no-probe penalty
2. Per-board surveys -- c6, xmc, k64f, rx72m (peripheral complement + coverage + weight)
3. Prioritized backlog per board
4. Cross-board shortlist -- peripherals that best exercise API neutrality
5. Surprising high-gain API-discovery candidates
6. Reconciliation -- what this changes in the scope doc + roadmap

---

## 1. Legend

Coverage (against the tree today: `arch/*/chip/*`, `user/driver/*`, `user/apps/*`):
- **driven** -- a real KickOS body drives it (kernel or userspace).
- **partial** -- touched for one narrow purpose (e.g. kernel tick, console ring), not a
  general driver / not the service-API shape.
- **absent** -- no KickOS code touches the block.

GAIN (each High / Med / Low):
- **api** -- API-discovery value: does driving it stress a service API in a NEW way, or
  likely FORCE a dedicated/new API? (the highest-value axis -- the point of the era.)
- **use** -- real-world usefulness of the block on this board.
- **xv** -- cross-vendor coverage value: does a sibling block exist on other matrix boards,
  so a driver here hardens a shared API against vendor variation?

COMPLEXITY (High / Med / Low, higher = MORE effort/cost):
- bring-up effort + spec depth + whether it needs DMA and/or IRQ plumbing.
- **no-probe penalty**: c6 and rx72m are flash-only, print-debug bring-up (no single-step,
  no register peek). Any block that cannot be diagnosed over the console costs MORE there;
  the penalty is folded into the c6/rx72m complexity ranks and called out where it dominates.

**P** = derived priority for M4 (1 = do first). Priority favours high api-gain at low
complexity, console/observability substrate first, and blocks already unblocked by an
existing template.

---

## 2. Per-board surveys

### 2.1 c6 -- ESP32-C6 (rv32imac / PMP + APM / Espressif; NO probe -> print-debug)
On-chip complement (HP domain unless noted; TRM + datasheet):
UART0/UART1 + LP-UART; GPSPI2 (+ SPI0/1 flash); I2C0 + LP-I2C; I2S; PARLIO (parallel IO);
GPIO/IO-MUX; GDMA (6ch, shared); RMT (TX/RX); LEDC (6ch PWM); MCPWM (1x motor);
PCNT (pulse counter); TWAI (CAN 2.0); USB-Serial/JTAG; SAR-ADC (12-bit, 7ch); temperature
sensor; ETM (event task matrix); systimer + TIMG0/1; MWDT/RWDT/super-WDT; RTC/LP-core;
crypto (AES/SHA/RSA/ECC/HMAC/DS) + TRNG; radios (Wi-Fi 6 / BLE 5 / 802.15.4). NO DAC on C6.

| Peripheral | Coverage | api | use | xv | cplx | P | One-line rationale |
|---|---|---|---|---|---|---|---|
| UART0 (console) | partial (kernel ring) | High | High | High | Med | 1 | Observability substrate -- MUST be first on a no-probe board; needs G5 APM open + reclaim body. |
| GPIO / IO-MUX | driven (c6blink) | High | High | High | Low | 1 | Canonical PMP+APM per-thread isolation proof already landed; the gpio/pinmux API reference. |
| GPSPI2 | absent | High | High | High | Med | 2 | The RISC-V/Espressif leg of the SPI call/reply API; different register model from DSPI/RSPI/USIC. |
| I2C0 | absent | High | Med | High | Med | 3 | First real I2C anywhere in the fleet -> first exercise of the addressed-transaction API. |
| LEDC (PWM) | absent | Med | Med | High | Low | 3 | Cheap, console-observable; PWM has NO service API yet -> discovery candidate (see 5). |
| RMT | partial (privileged WS2812) | High | Med | Low | Med | 4 | Symbol/timing engine -- fits no existing API; strong dedicated-API discovery, low xv. |
| SAR-ADC + temp sensor | absent | High | Med | High | Med | 3 | Analog-in exists on ALL four boards but has NO API -> high neutrality value (see 4/5). |
| GDMA | absent | High | Med | High | High | 5 | The shared-DMA isolation problem (scope 3.4); central allocator, not a grant. Defer. |
| TWAI (CAN) | absent | Med | Med | High | High | 5 | CAN on all four -> good xv, but heavy framing + no bench CAN traffic; defer. |
| USB-Serial/JTAG | partial (boot/console path) | Low | Med | Low | High | -- | The flash/console channel; a full USB device stack is out of era scope. |
| MCPWM / PCNT / PARLIO / I2S | absent | Med | Low | Med | High | -- | Niche; PARLIO/I2S want GDMA first. Park. |
| ETM | absent | High | Low | Med | Med | -- | HW event routing -- no API analog (see 5); park behind a forcing consumer. |
| crypto + TRNG | absent | Med | Low | Med | Med | -- | TRNG is a cheap entropy-source API seed; full crypto out of scope. |
| WDT / RTC / radios | partial/absent | Low | -- | -- | -- | -- | WDT partly kernel-side; radios are a stack, not a driver-era peripheral. |

### 2.2 xmc -- XMC4800 (armv7m / PMSAv7 / Infineon; probe OK)
On-chip complement (RM): USIC0/1/2 (2 ch each -- each channel = UART/SPI/I2C/I2S/LIN);
CCU4 (4x timer/PWM) + CCU8 (2x PWM w/ complementary + dead-time); POSIF (motor position);
VADC (12-bit, multi-group SAR) + DSD (delta-sigma demod); DAC (2ch); MultiCAN+ (up to 6
nodes); ETH MAC (10/100); **ECAT (on-chip EtherCAT slave controller)**; USB FS OTG; SDMMC;
EBU (external bus); GPDMA (2x) + DMA linked to peripherals; DTS (die temp sensor); RTC; WDT;
ERU (event request unit); FCE (CRC); PORTS/GPIO; SCU.

| Peripheral | Coverage | api | use | xv | cplx | P | One-line rationale |
|---|---|---|---|---|---|---|---|
| USIC UART (U0C0) | driven (xmcuart) | -- | High | High | -- | -- | The console-driver template + reclaim reference; done. |
| USIC SPI | driven (xmcspi) | Med | High | High | Low | 1 | The channel-shared USIC as SPI; the MUX-free-window CS gate lives here (scope 6). |
| GPIO / PORTS | driven (xmcspi CS) | High | High | High | Low | 1 | The port-granular / IOCR-inseparable case -- the atomic set/clear window homework. |
| USIC I2C | absent | High | Med | High | Med | 2 | Same USIC block as UART/SPI -> tests whether ONE mode-select API spans three roles (see 5). |
| CCU4 / CCU8 | partial (CCU4 = kernel tick) | High | High | Med | Med | 3 | PWM/timer with dead-time -- no PWM API yet; CCU4 already a kernel timer, generalise the seam. |
| VADC + DTS | absent | High | Med | High | Med | 3 | Multi-group ADC + die temp -> the analog-in neutrality leg (see 4). |
| ECAT (EtherCAT SC) | absent | High | High | Med | High | 4 | On-chip fieldbus slave -- forces a wholly new API; KickCAT-adjacent (see 5). Probe helps. |
| MultiCAN+ | absent | Med | Med | High | High | 5 | CAN xv leg; multi-node adds mailbox complexity. Defer. |
| ETH MAC | absent | High | Med | Med | High | 5 | Net stack -- forces a new API class but heavy; defer to a net sub-era. |
| GPDMA | absent | High | Med | High | High | -- | Shared-DMA isolation (scope 3.4); central allocator. Defer with c6 GDMA. |
| DAC | absent | Med | Low | Med | Low | 4 | Analog-OUT; cheap; only xmc+k64f+rx72m have it (not c6) -> partial-fleet API probe. |
| ERU | absent | High | Low | Med | Med | -- | Event-router sibling of C6 ETM / RX ELC (see 5); park behind a consumer. |
| USB / SDMMC / EBU / POSIF / FCE / RTC / WDT | absent/partial | Low-Med | Low | Low | High | -- | Out of era scope or niche; WDT/RTC partial kernel-side. |

### 2.3 k64f -- FRDM-K64F (armv7m / SYSMPU / NXP; probe OK, OpenSDA/J-Link)
On-chip complement (RM): UART0-5 (0/1 fast); SPI0-2 (DSPI); I2C0-2; FTM0-3 (FlexTimer
PWM/motor); PIT; LPTMR; PDB (programmable delay); CMP (analog comparators); ADC0/1 (16-bit
SAR); DAC0/1 (12-bit); FlexCAN (CAN0); ENET (10/100 + IEEE1588); USB FS OTG; SDHC; RTC;
eDMA (16ch) + DMAMUX; GPIO PORTA-E; CRC; RNGA; SYSMPU; SAI/I2S; LLWU; EWM/WDOG.
(FRDM board: FXOS8700 accel/mag on I2C0.)

| Peripheral | Coverage | api | use | xv | cplx | P | One-line rationale |
|---|---|---|---|---|---|---|---|
| UART0 (console) | partial (kernel ring + sync; reclaim body ready) | High | High | High | Low | 1 | End-to-end handover NEVER run -- proves the already-written reclaim body; needs a userspace driver. |
| DSPI (SPI0) | driven (k64dspi + k64drv) | Med | High | High | -- | 1 | The EOQ-IRQ SPI driver + KickCAT-over-SPI precedent; the SPI-API baseline. |
| GPIO PORTA-E | driven (CS via PORTB/C) | High | High | High | Low | 1 | Coarse-AIPS (isolation = doc only) -> the contrast case for the gpio-cap model. |
| I2C0 | absent | High | High | High | Med | 2 | On-board FXOS8700 is a REAL I2C client -> the addressed-transaction API with real traffic. |
| FTM0-3 | absent | High | High | High | Med | 3 | FlexTimer PWM/motor -- the NXP leg of the (missing) PWM API; xv with CCU/GPT/MTU. |
| ADC0/1 + CMP + DAC | absent | High | Med | High | Med | 3 | 16-bit ADC + comparators + DAC -> analog neutrality leg; CMP has no analog to c6/xmc (discovery). |
| FlexCAN | absent | Med | Med | High | High | 5 | CAN xv leg; mailbox framing. Defer. |
| ENET | absent | High | Med | Med | High | 5 | Net class; heavy. Defer to net sub-era. |
| eDMA + DMAMUX | absent | High | Med | High | High | -- | Shared-DMA isolation (scope 3.4); DMAMUX = the channel-allocator model. Defer. |
| RNGA | absent | Med | Low | Med | Low | 4 | Cheap entropy-source API seed, xv with c6 TRNG / rx TSIP. |
| PDB / PIT / LPTMR | partial (kernel timer) | Med | Low | Med | Med | -- | Timer variants; kernel already uses one. Park. |
| SAI/I2S / SDHC / USB / RTC / WDOG | absent/partial | Low | Low | Low | High | -- | Out of era scope / partial kernel-side. |

### 2.4 rx72m -- RX72M (RXv3 / RX-MPU / Renesas; NO probe -> print-debug)
On-chip complement (RM/datasheet): SCI0-12 (async/sync/simple-SPI/simple-I2C -- up to 13);
RSPI (2x true SPI); RIIC (3x I2C); MTU3 (MTU0-8, multi-function timer/PWM); GPTW0-3+ (general
PWM timer); TMR (8-bit); CMT/CMTW; TPU; POE3; S12AD (12-bit ADC, 2 units); DAC (12-bit, 2ch);
**EtherCAT slave controller (ESC)**; ETHERC + EDMAC (Ethernet); CAN (+ CANFD on variants);
USBa FS; SDHI; QSPIX; DMAC (8ch) + DTC + EXDMAC; RTC; WDT/IWDT; ELC (event link controller);
CRC; TSIP/RSIP (secure crypto + TRNG); temp sensor; CMOS camera IF; GLCDC + 2D DRW.

| Peripheral | Coverage | api | use | xv | cplx | P | One-line rationale |
|---|---|---|---|---|---|---|---|
| SCI6 (console) | partial (kernel ring) | High | High | High | Med | 1 | Observability substrate on a no-probe board; needs G5 RX IRQ-demux + reclaim body. |
| GPIO / PORTS | driven (rxdrv blink) | High | High | High | Low | 1 | RX-MPU per-thread isolation leg; the RXv3 gpio-cap case. |
| RSPI | absent (rxdrv touches spi) | High | High | High | Med | 2 | The Renesas/RXv3 leg of the SPI call/reply API; distinct from DSPI/USIC/GPSPI. |
| RIIC | absent | High | Med | High | Med | 3 | I2C xv leg; RIIC has its own quirks vs USIC/K64-I2C/GPSPI. |
| SCI-as-SPI / SCI-as-I2C | absent | High | Med | Med | Med | 4 | ONE SCI block, many modes -> tests a mode-select API (twin of the USIC question, see 5). |
| MTU3 / GPTW | partial (one = kernel timer) | High | High | Med | Med | 3 | Two distinct PWM/timer units on ONE chip -> stresses a PWM API's instance/variant model. |
| S12AD + temp + DAC | absent | High | Med | High | Med | 3 | Analog neutrality leg; temp sensor cross-checks the c6/xmc/k64f temp API (see 4). |
| ESC (EtherCAT SC) | absent | High | High | Med | High | 4 | Second on-chip fieldbus slave in the fleet (with xmc) -- forces a new API; KickCAT-adjacent; no-probe cost. |
| ELC | absent | High | Low | Med | Med | -- | Event-link sibling of C6 ETM / XMC ERU (see 5); park behind a consumer. |
| CAN / CANFD | absent | Med | Med | High | High | 5 | CAN xv leg; CANFD adds a frame-format axis. Defer. |
| ETHERC + EDMAC | absent | High | Med | Med | High | 5 | Net class; heavy; no probe. Defer to net sub-era. |
| DMAC + DTC + EXDMAC | absent | High | Med | High | High | -- | Shared-DMA isolation (scope 3.4); THREE transfer engines -> rich allocator test, but defer. |
| TSIP (TRNG) | absent | Med | Low | Med | High | -- | Entropy xv, but TSIP is a heavy secure block -- poor effort/gain vs RNGA/C6-TRNG. |
| QSPIX / SDHI / USBa / GLCDC / camera / RTC / WDT | absent/partial | Low | Low | Low | High | -- | Out of era scope / niche / partial kernel-side. |

---

## 3. Prioritized backlog per board (do-first order)

- **c6**: (1) UART0 console driver + APM open + reclaim, GPIO already done; (2) GPSPI2;
  (3) I2C0 / SAR-ADC+temp / LEDC; (4) RMT; (5) GDMA / TWAI (defer). No-probe: everything
  above the SPI line must print its own bring-up state.
- **xmc**: (1) USIC-SPI + GPIO (done/near), console done; (2) USIC-I2C (the three-modes-one-
  block probe); (3) CCU4/CCU8 PWM + VADC/DTS; (4) ECAT / DAC; (5) MultiCAN+/ETH/GPDMA (defer).
- **k64f**: (1) UART0 userspace console (prove the reclaim body) + DSPI/GPIO (done);
  (2) I2C0 (real FXOS8700 traffic); (3) FTM PWM + ADC/CMP/DAC; (4) RNGA; (5) FlexCAN/ENET/
  eDMA (defer).
- **rx72m**: (1) SCI6 console driver + RX IRQ-demux + reclaim, GPIO (near); (2) RSPI;
  (3) RIIC / MTU3+GPTW / S12AD+temp; (4) SCI-as-SPI/I2C + ESC; (5) CAN/ETHERC/DMAC (defer).
  No-probe: same print-first discipline as c6.

Cross-board sequencing follows `design-driver-era-scope.md` section 5: console-first
everywhere (observability), then the bus drivers on the call/reply layer, then the analog/PWM
classes that force NEW APIs, DMA/net/CAN last.

---

## 4. Cross-board shortlist -- best exercisers of API neutrality

Ranked by how hard each is to shape neutrally across all four vendors (a leaked vendor
assumption cannot survive the spread):

1. **Console/UART on all four** -- USIC(ch) / K64-UART / GPSPI-sibling UART / SCI: four
   register models, four baud derivations, four reclaim register-sets, four FIFO vs single-
   datum shapes. The one class that DOES want all four boards (no-probe forces it). Already
   the spine of the scope doc.
2. **SPI: USIC-SPI (xmc) + DSPI (k64f) + RSPI (rx72m) + GPSPI2 (c6)** -- four genuinely
   different SPI IPs across four vendors AND four arch/MPU families; the single strongest test
   of the call/reply transfer API + the CS/GPIO-cap model. Map one per board, do NOT duplicate.
3. **I2C: USIC-I2C / K64-I2C / RIIC / GPSPI-sibling I2C0** -- first addressed-transaction API
   in the fleet; k64f's on-board FXOS8700 gives REAL bus traffic to validate against.
4. **Analog-in (ADC + temp sensor): SAR-ADC (c6) / VADC+DTS (xmc) / ADC0-1 (k64f) /
   S12AD+temp (rx72m)** -- ALL FOUR have both an ADC and a die-temp sensor, yet KickOS has NO
   analog API. A temp-sensor read is cheap and console-observable (works on the no-probe
   boards) -> the ideal low-cost neutrality smoke-test for a brand-new API class.
5. **PWM/timer: CCU4/CCU8 (xmc) / FTM (k64f) / MTU3+GPTW (rx72m) / LEDC+MCPWM (c6)** -- four
   deeply different timer/PWM architectures (rx72m alone has TWO); no PWM API exists -> forces
   the instance/variant/dead-time shape of a new API.

Console + one-bus-per-board is the MINIMUM neutrality proof already in the roadmap; the
analog-in and PWM legs are the additions this survey argues are high-gain at moderate cost.

---

## 5. Surprising high-gain API-discovery candidates

These are NOT in the service-class list, and each is a strong reason the scope is now an
aspiration-to-full-coverage rather than a bounded set:

- **The die temperature sensor (all four boards).** Trivial to read, console-observable so it
  works on the no-probe boards, and present on EVERY matrix board -- yet it fits no existing
  API. It is the cheapest possible forcing function for a first "analog/sensor read" service
  API, and being 4/4 it is an unusually clean neutrality test for a new class. Highest
  gain-per-effort on the board.
- **The event-interconnect fabric: C6 ETM / XMC ERU / RX ELC.** Three vendors ship a HW block
  that routes a peripheral event to another peripheral's task WITHOUT CPU involvement (ADC-
  trigger-on-timer, DMA-on-compare). This maps onto NO service abstraction KickOS has, and it
  interacts with the capability model in a new way (who may wire whose events?). A genuine
  "this hardware needs a new API" discovery -- exactly the class the full-coverage aspiration
  is meant to surface. Park behind a forcing consumer, but flag it now.
- **On-chip EtherCAT slave controller (xmc ECAT + rx72m ESC).** TWO of four boards carry a
  hardware fieldbus slave on-die, and KickCAT is already the flagship KickOS consumer
  (`design-kickcat-k64f.md`, today an SPI-attached ESC on k64f). Driving the on-chip ESC on
  xmc/rx72m forces a wholly new fieldbus API AND lets KickCAT run without an external ESC --
  high real-world and API-discovery value, gated only by the heavier bring-up (and the
  no-probe cost on rx72m).
- **A single serial block that is UART, SPI and I2C by mode: XMC USIC and RX SCI.** Both
  vendors fold three of the service classes into ONE configurable block. This is a pointed
  test of whether the console/spi/i2c APIs can share a mode-select seam, or whether a per-role
  API accidentally forces three separate drivers over identical registers -- a neutrality trap
  an all-separate-blocks vendor (k64f) would never surface.
- **Entropy source: C6 TRNG / K64 RNGA / RX TSIP.** A one-call "get random bytes" API is cheap
  on c6/k64f and would seed a fleet-wide entropy service; only rx72m's TSIP is heavy. Low
  effort on three of four -> a quick new-API win.

---

## 6. Reconciliation

This survey's weighting is the concrete BOUND the maintainer's revised scope refers to.
`roadmap.md` (M4 section, the scope-guard bullet) and `design-driver-era-scope.md` (7.2, the
driver->board mapping principle) are updated to state the aspiration as FULL per-board
coverage PRIORITIZED by this matrix, not a set restricted to the service-defined classes. The
milestone-discipline spirit is preserved: the weighting -- console-first, high-api-gain at low
complexity next, DMA/net/CAN deferred -- is what keeps the era from ballooning into a
four-vendor BSP, exactly as an a-priori class restriction previously did, but WITHOUT hiding
the peripherals whose coverage is the point (analog-in, PWM, the event fabric, the on-chip
ESC).
