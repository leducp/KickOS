<!-- SPDX-License-Identifier: CECILL-C -->
# Design brief: K64F/DSPI unprivileged userspace SPI driver

Design only; no code. The SPI driver KickCAT actually needs: a DSPI master on the
FRDM-K64F driving an EtherCAT Slave Controller (ESC) over its SPI PDI, feeding the
KickCAT slave stack. Counterpart of `design-spi-driver.md` (XMC/USIC-SSC), deferred to
this pass by that brief's TARGET CORRECTION note. Builds on the LANDED MMIO-grant seam
(`design-task9-mmio-driver.md`; `kos_thread_params.mmio_base/mmio_size`), the tier-1 IRQ
path (`kos_irq_register/wait/ack`), and the silicon-proven k64drv AIPS-open pattern
(`user/apps/k64drv/main.cc`).

## The hardware ceiling this brief designs WITHIN (read first)
K64F peripheral isolation is **AIPS-PACR-based, NOT SYSMPU** -- silicon-proven via the
k64drv PIT driver. The SYSMPU is a bus-slave-side unit: it guards flash/SRAM crossbar
ports and NEVER sees a peripheral-bridge access, so it cannot gate the DSPI at all. The
AIPS peripheral bridge gates by **privilege level + bus master, per 4 KB slot**, and
each slot's `PACR` `SP` (supervisor-protect) field resets to supervisor-only. See
`docs/reference/architecture.md` (Memory domains -- the peripheral-MMIO matrix) and the
worked bring-up in `docs/book/peripheral-isolation-and-the-hardware-ceiling.md`.

Consequence, stated honestly: once the privileged shim clears the DSPI slot's `PACR SP`
bit, the DSPI registers are reachable by **every** unprivileged thread in the system,
at whole-4-KB-slot granularity. The driver is isolated from the kernel and from other
domains' MEMORY (SYSMPU still enforces SRAM domains, 17/17), but there is **no
per-thread peripheral boundary** on K64F. On XMC PMSA / RISC-V PMP the identical MMIO
grant IS a genuine per-thread capability (the unit sees MMIO from the CPU side); on
K64F it is not, and no kernel code recovers that. The user's decision stands: **option
(a) -- keep the microkernel invariant (drivers in userspace)** and accept the coarser
K64F peripheral ceiling, documented, rather than mediate every DSPI access through a
syscall.

## Confirmed hardware facts (K64 RM Rev.4 Oct 2019; FRDM-K64F)
Instance: **SPI0 (DSPI0)** -- the FRDM-K64F Arduino R3 header SPI, the natural landing
for an ESC daughterboard/shield.

| Fact | Value | Citation |
|---|---|---|
| DSPI0 base | `0x4002_C000` | RM memory map (SPI0 = slot 44); register map 50.3 p1481 |
| AIPS0 slot | 44 | RM peripheral-bridge memory map (`0x4002_C000` -> slot 44) |
| AIPS PACR reg | `AIPS0_PACRF` @ `0x4000_0044` | RM 20.2 p450 (PACRF covers slots 40-47) |
| DSPI0 PACR field | PACR44 = bits `[15:12]`; `SP` = **bit 14** | RM 20.2.2 (field map) + field desc (bit 30-of-nibble = Supervisor Protect) |
| Clock gate | `SIM_SCGC6` @ `0x4004_803C`, `SPI0` = **bit 12** | RM 12.2.13 p325 (field desc: "SPI0 Clock Gate Control") |
| NVIC IRQ | **26** (vector 42, `0x0000_00A8`); single vector, all sources | RM ch.3 vector table |
| Pins (ALT2 of PORTD) | PTD0=`SPI0_PCS0`, PTD1=`SPI0_SCK`, PTD2=`SPI0_SOUT`, PTD3=`SPI0_SIN` | RM signal-mux table (PORTD rows; SPI0 on ALT2) |

Register offsets from base (RM 50.3):

| Reg | Off | Reset | Key fields |
|---|---|---|---|
| `MCR`   | `0x00` | `0000_4001h` | MSTR b31, CONT_SCKE b30, FRZ b27, PCSIS `[21:16]`, MDIS b14, DIS_TXF b13, DIS_RXF b12, CLR_TXF b11, CLR_RXF b10, SMPL_PT `[9:8]`, HALT b0 |
| `TCR`   | `0x08` | 0 | transfer counter |
| `CTAR0` | `0x0C` | `7800_0000h` | DBR b31, FMSZ `[30:27]`, CPOL b26, CPHA b25, LSBFE b24, PCSSCK `[23:22]`, PASC `[21:20]`, PDT `[19:18]`, PBR `[17:16]`, CSSCK `[15:12]`, ASC `[11:8]`, DT `[7:4]`, BR `[3:0]` |
| `SR`    | `0x2C` | `0200_0000h` | TCF b31, TXRXS b30, EOQF b28, TFFF b25, RFDF b17 (all w1c) |
| `RSER`  | `0x30` | 0 | TCF_RE b31, EOQF_RE b28, TFFF_RE, RFDF_RE |
| `PUSHR` | `0x34` | 0 | CONT b31, CTAS `[30:28]`, EOQ b27, CTCNT b26, PCS `[21:16]`, TXDATA `[15:0]` |
| `POPR`  | `0x38` | 0 | RXDATA `[15:0]` (read-only) |
| `TXFR0..3` | `0x3C..0x48` | 0 | FIFO shadow (RO) |
| `RXFR0..3` | `0x7C..0x88` | 0 | FIFO shadow (RO) |

MCR reset `0000_4001h` = **MDIS=1, HALT=1** at reset: the module boots disabled and
halted; the shim must clear MDIS, flush FIFOs (CLR_TXF|CLR_RXF), then release HALT.

**Could NOT confirm from the extractable RM/UG text (flag for bench):** the FRDM-K64F
header J-connector PIN NUMBERS. The UG pinout (Fig.18) is an image, not text. The
board's Arduino-R3 SPI is the standard PTD0-3 set -- D13=SCK=PTD1, D12=MISO=PTD3(SIN),
D11=MOSI=PTD2(SOUT), D10=CS=PTD0(PCS0) -- but confirm the exact J2 pin positions against
`FRDM-K64F-SCH` at bring-up (as the console did for PTB16/17). The PORT/pin/ALT2 mapping
itself IS RM-confirmed. Alternative SPI0 pin set PTC4-7 (also ALT2) exists but is not the
shield header; PTD0-3 is the recommendation.

## Organizing principle (and its K64F caveat)
On a CPU-side-MPU chip the granted DSPI window IS the security boundary. On K64F it is
NOT: the SYSMPU RGD for the DSPI window is **INERT for peripheral isolation** -- the
AIPS-opened slot 44 is the real (and only) peripheral authority, and it admits all user
code. So the K64F boundary is **kernel-vs-user, per-slot**, drawn by whether the shim
opens the slot -- not per-thread, and not per-register-window. The grant is still issued
(it keeps the driver's region set coherent and is exactly what a PMSA/PMP port would
enforce), but on THIS silicon it is documentation, not enforcement.

The escalation surfaces stay privileged and out of the driver's reach regardless:
**SIM clock gates** (`SIM_SCGC6` -- could ungate any peripheral) and **PORTD pin-mux**
(`PORTD_PCRn` -- could re-mux SPI onto or off arbitrary pins). The driver never gets the
SIM or PORT windows -- only the DSPI register window + the DSPI IRQ.

## Privilege split (mirror `kickos_xmc_usic_init` ordering + k64drv)
Privileged boot, once, in the bring-up shim (this `main` runs privileged):
1. `SIM_SCGC6 |= (1<<12)` -- clock-gate DSPI0 (also brings its AIPS slot alive).
2. Pin-mux LAST-safe order: program `PORTD_PCR0..3` MUX=ALT2 for PCS0/SCK/SOUT/SIN.
   SCK/SOUT are outputs -- program the controller to a defined idle (CPOL) BEFORE the
   mux takes the pins, to avoid an idle-edge glitch on the ESC clock line (the XMC brief
   makes the same "mux last" point).
3. DSPI config while halted:
   - `MCR`: MSTR=1 (master), HALT=1 held during config, CLR_TXF=1|CLR_RXF=1 (flush),
     MDIS=0 (module on), PCSIS bit 16 set = PCS0 inactive-high (ESC CS idle high),
     DIS_TXF=0/DIS_RXF=0 (FIFOs enabled).
   - `CTAR0`: FMSZ = frame-size-1 (start 8-bit -> FMSZ=7), CPOL/CPHA per the ESC's SPI
     mode (LAN9252/ET1100 SPI PDI is mode 0 or mode 3 -- an ESC-specific boot constant,
     not a driver runtime knob), LSBFE=0 (MSB-first, ESC default), PBR/BR baud +
     PCSSCK/PASC/PDT + CSSCK/ASC/DT chip-select and inter-frame delays sized to the ESC
     datasheet's SPI timing.
   - Release: `MCR.HALT=0` (or leave halted and let the first PUSHR/EOQ cycle drive it).
4. **Open the slot to user mode:** `AIPS0_PACRF &= ~(1u<<14)` -- clear DSPI0 slot-44
   `PACR SP`. Compute exactly as k64drv did for PIT slot 55 (there PACRG bit 2); here
   slot 44 -> PACRF field `[15:12]` -> `SP` = bit 14.
5. `RSER = EOQF_RE (1<<28)` -- route the End-Of-Queue flag to NVIC IRQ 26.
6. Spawn the UNPRIVILEGED driver: `mmio_base = 0x4002_C000`, `mmio_size = 0x100`
   (covers `MCR..RXFR3` at `0x00..0x88`; see budget), granted DSPI IRQ line 26 via the
   tier-1 path. Everything after boot is in-window MMIO + IRQ wait.

## Chip-select: HARDWARE PCS (recommended)
Use hardware `PCS0` (PTD0): assert via `PUSHR.PCS` bit 16, hold across a multi-frame
ESC command with `PUSHR.CONT=1` (bit 31), release by clearing CONT on the last frame.
CS timing (PCS-to-SCK `CSSCK`, after-SCK `ASC`, delay-after-transfer `DT`) is CTAR0,
set once at boot. This is entirely in the DSPI window -- **zero extra regions, zero
GPIO**. Inactive polarity is `MCR.PCSIS` bit 16 (CS idle-high for the ESC).

GPIO-CS alternative (rejected): would need a PORTD GPIO data register + direction, a
second granted window, and -- worse on K64F -- it re-introduces the PORT block whose PCR
mux is the escalation surface we deliberately keep privileged. HW PCS is strictly
cleaner and folds CS into the one DSPI window.

## Transfer model: IRQ-driven, tier-1 reused unchanged
Single-word full-duplex first (RX-complete implies TX shifted; the FIFO burst is a later
optimization). Per `spi_transfer`:
- push one frame: `PUSHR = CONT? | (CTAS=0 -> CTAR0) | EOQ(bit27) | PCS(bit16) | tx16`;
- park on the DSPI IRQ (`kos_irq_wait`) -- the tier-1 first-level ISR masks line 26 and
  posts the driver's notification; polling would burn the unprivileged quantum;
- on wake, read `POPR` (RX16), then **W1C `SR.EOQF` (write `1<<28`) BEFORE the line is
  re-armed** -- EOQF/TCF are w1c; an un-cleared flag re-asserts the level on unmask and
  STORMS the line (the exact PIT-TIF hazard k64drv hit and the XMC brief calls out). The
  re-arm is the loop's next `kos_irq_wait` (auto-re-arm); an explicit `kos_irq_ack` is now
  OPTIONAL (early re-arm for the compute-then-wait shape) -- either way, W1C comes first.

**EOQ over TCF (recommended).** TCF (bit 31) fires per-frame-complete -> N interrupts for
an N-frame ESC command. EOQ (mark the last/only frame's `PUSHR.EOQ`, wait `SR.EOQF`)
fires **once per logical transfer** regardless of frame count, so the IRQ model does not
change when the FIFO-burst optimization lands -- push N, EOQ the last, one wake. Note the
side effect: setting EOQF auto-clears `SR.TXRXS` (the module STOPS/RUNNING->STOPPED at
end-of-queue); the driver clears EOQF and the next `PUSHR` restarts the queue -- which
matches a request/response ESC exchange exactly. NVIC line 26 is a **single vector for
all DSPI0 sources** (RM), so only the enabled `RSER` bit (EOQF_RE) decides what wakes it.

## App-facing API (faithful to "write a main, that's it")
```
int spi_transfer(void* tx, void* rx, size_t len);   // blocking
```
Blocking: enqueues the (tx, rx, len) descriptor to the driver thread, blocks on a
semaphore, returns bytes transferred or <0. The app NEVER touches MMIO, grants, or IRQs.
The driver thread is the spawned unprivileged owner of the DSPI window + IRQ handle,
started by the small privileged bring-up shim above; its stack is pow2-defined
(`KOS_STACK_DEFINE`) so a PMSA/PMP sibling port encodes it in one region. KickCAT sits on
top: the driver provides only `spi_transfer`; the ESC PDI/SPI framing (register
addressing, read/write command bytes, wait states) is entirely KickCAT's concern.

## Region / slot budget
K64F SYSMPU is byte-granular with ~12 RGDs -- ample. code + appdata + stack + DSPI window
= **4 of ~12** (HW CS folded in, no GPIO region). The DSPI-window RGD is (as noted) inert
for peripheral isolation on K64F but still spent for region-set coherence and PMSA/PMP
portability. `mmio_size = 0x100` clears the byte-granular encodability check
(`design-task9-mmio-driver.md`) trivially; a PMSA port would need pow2 >= 32 and natural
alignment -- `0x100` @ `0x4002_C000` is pow2 and 256-aligned, so the SAME grant is
encodable on the CPU-side-MPU flavors too.

## Open questions / risks
1. **No per-thread peripheral isolation on K64F** (the ceiling, not a bug): once slot 44
   is open, any unprivileged thread reaches DSPI0. If KickCAT ever needs driver-A-vs-B
   DSPI isolation on THIS board, the only route is syscall-mediation -- out of scope here.
2. **Fault-vs-grant decode:** an ungranted / still-supervisor-only DSPI access surfaces
   as a **BusFault** on K64F (AIPS error response), NOT a SYSMPU MemManage -- the fault
   reporter must decode the bus-fault path + print the address (k64drv proved the fault
   fires; confirm the reporter's decode on the DSPI address).
3. **ESC SPI mode (CPOL/CPHA) + baud** are ESC-part-specific boot constants -- pin them
   to the chosen ESC datasheet (ET1100 vs LAN9252 differ), not guessed.
4. **FRDM header J-pin numbers** unconfirmed from text (Fig.18 image) -- confirm against
   `FRDM-K64F-SCH` at bench.
5. **EOQ STOPPED-state re-arm:** verify on silicon that W1C EOQF + next PUSHR cleanly
   restarts the queue for back-to-back ESC exchanges (RUNNING<->STOPPED transition).
6. **Full-duplex length > FIFO depth** (DSPI TX FIFO = 4 on SPI0): single-word model is
   immune; the burst optimization must respect the 4-entry depth (TFFF/RFDF pacing).

## Sequencing
Builds entirely on landed pieces: MMIO-grant seam (task #9) + k64drv-proven AIPS-open +
tier-1 IRQ. Order: (1) privileged shim -- clock/mux/config/PACR-open, (2) unprivileged
driver thread + `spi_transfer`, (3) KickCAT ESC link on top. Pure addition, no kernel
change: the transfer path is in-window MMIO + the existing IRQ-as-event path; the only
device-specific need is W1C EOQF before the re-arm (the next `kos_irq_wait`, or the optional
early `kos_irq_ack`). Prove the raw `spi_transfer` (loopback
SOUT->SIN, or a scope on PTD1/PTD2) before wiring the ESC.
