<!-- SPDX-License-Identifier: CECILL-C -->
# Design brief: unprivileged userspace SPI driver

> **TARGET CORRECTION (read first).** The SPI driver KickCAT actually needs is on the
> **K64F (DSPI)** -- the goal is a REAL driver with REAL usage *before* M3, feeding the
> KickCAT slave stack. So the implementation target is **K64F/DSPI**, and resolving
> **SYSMPU peripheral-bridge gating under user mode** (unproven -- commit 033b570; see
> below) is now **critical-path, not optional**: the K64F GPIO/timer first-driver
> bring-up MUST confirm SYSMPU actually isolates peripherals, or K64F MMIO isolation is
> illusory. The XMC/USIC-SSC design below stays valuable as the **M5** board-support /
> userspace-library ("piles") reference and as the PMSA-proven fallback, but a future
> subagent (post-context-clear) should produce the **K64F/DSPI** counterpart of this
> brief (privilege split, the DSPI register window as the granted MMIO region, hardware
> PCS vs a GPIO CS, the DSPI EOQ/TCF completion IRQ) reusing the same MMIO-grant model.
>
> **DONE (2026-07-16):** that counterpart is now `design-spi-driver-k64f-dspi.md` --
> SPI0/DSPI0 on the FRDM-K64F Arduino header (PTD0-3), HW PCS, EOQ completion,
> single-word-first. It also records the silicon-established ceiling the SYSMPU note
> below was blocked on: K64F peripheral isolation is **AIPS-PACR-based, not SYSMPU**, so
> the DSPI window grant is a genuine per-thread capability only on the CPU-side-MPU
> flavors (XMC PMSA / RISC-V PMP), NOT on K64F (kernel-vs-user, per-4-KB-slot).

**IMPLEMENTED (build-only, pending silicon) 2026-07-17:** landed as `user/apps/xmcspi/`
on **U0C1** (USIC0 channel 1, 0x4003_0200) -- the console keeps U0C0. Loopback is
INTERNAL (RM 18.2.3.5 Loop Back Mode: DX0 input stage selects on-chip input "G" = the
channel's own transmitter), so **no port pins are muxed and no MISO<->MOSI jumper is
needed**. Granted window = 512 B `R|W|DEV` no-X (pow2, 0x200-aligned -> encodable).
Receive-complete IRQ = USIC0 **SR1 -> NVIC 85** (SR0 is the console TX); a single-word
frame is SOF=1 so the completion flag is **AIF** (alternative receive), not RIF (RM
18.4.2.7) -- the driver arms + W1Cs both. Negative test pokes the UNGRANTED **SCU**
clock-gate register (0x5000_4648) -> MemManage. Builds clean under
`-DKICKOS_HAVE_MPU=1` (preset `xmc4800-relax-st`). Open questions 1-5 below to be
answered on silicon by the operator.

Extends `design-task9-mmio-driver.md`
(privileged-only MMIO grant; MMIO = a Domain region `R|W|DEV` no-X; Option-A
grant-at-spawn via `kos_thread_params`; `KICKOS_MPU_MAX_REGIONS = 8`) and reuses the
two-tier IRQ path (`irq_register`/`irq_wait`/`irq_ack`) unchanged.

## Chip: XMC4800, USIC in SSC (SPI) mode
PMSA `DEV` attr is enforcement-proven on this silicon, and the USIC block is already
modeled clean-room (`arch/arm/chip/xmc4800/usic.h` + `usic_uart.cc` -- SSC is a
protocol layer mirroring the ASC/UART layer, reusing clock/baud/mux/FIFO/IRQ ops).
K64F/DSPI deferred: SYSMPU peripheral-bridge gating under user mode is unproven
(commit 033b570 flags it) -- do not lead SPI on an unproven isolation backend.

## Organizing principle
The granted USIC-channel MMIO window IS the security boundary, not the semantics of
the registers inside it. Everything in-window only affects THIS channel on its
ALREADY-assigned pins (baud, frame, mode, CS) -- contained, not an escalation. The
operations that reach OTHER pins/peripherals stay privileged and OUT of the window:
the port pin-mux (IOCR) and the SCU clock gate.

## Privilege split
- Privileged boot (once): SCU clock ungate+de-reset; baud/prescaler; controller
  mode/frame (`CCFG.SSC`, `CCR.MODE=SSC`, `SCTR`, `PCR` master + CS policy); pin-mux
  (IOCR for SCLK/MOSI/MISO/CS + `DXnCR` input select) -- programmed LAST to avoid an
  idle-level glitch. Mirrors `kickos_xmc_usic_init()` order, then hands the channel
  window to the driver via the grant.
- Unprivileged driver (per transfer): load TX (`TBUF`/FIFO), read RX (`RBUF`), start,
  wait completion, W1C the SSC flag (`PSCR`), assert/deassert hardware CS (`PCR`
  SELO) -- all in-window.

Escalation surfaces kept privileged + out-of-window: **SCU** (system clock tree --
could ungate any peripheral) and **port IOCR** (pin-mux -- could steer SPI onto
arbitrary pins / capture others' pins).

## The MMIO grant: ONE channel window
- USIC channels are 0x200 apart (U0C0=0x40030000, U0C1=0x40030200, U1C0=0x48020000,
  U2C0=0x48024000). Grant = base=channel base, size=**0x200 (512 B)**, `R|W|DEV` no-X.
- PMSA-clean (reject-not-round): 512 is pow2 >= 32 B min, and every channel base is
  0x200-aligned -> one descriptor, no pad/split. Minimal window: NOT the whole USIC,
  NOT the peripheral bridge, NOT IOCR/SCU.
- Console owns U0C0; SPI uses a different channel (e.g. U0C1) -- exact channel/pins
  are a board-header detail (pin against the Relax Kit board manual, like the console
  P1.4/P1.5, not guessed).

### Chip-select is the key SPI-specific choice
- **Hardware CS (SELOx) recommended** -- CS enable/timing is in-window (`PCR.MSLSEN`/
  `SELO`/`SELINV`); zero extra regions.
- **GPIO CS rejected on XMC:** it needs the port data reg (port+0x00/0x04) but IOCR
  sits at port+0x10; PMSA's 32-byte minimum region from the port base spans 0x00-0x1F
  and so exposes IOCR -> pin-remux escalation. Cannot split data from mux at 32 B
  granularity here. Hardware CS sidesteps it.

Region budget (ARMv7-M PMSA, 8 max): code 1 + appdata 1 + stack 1 + SPI window 1 =
**4/8** (CS folded in). Cheaper than the task #9 GPIO+timer example (5).

## Transfer model: IRQ-driven, tier-1 reused unchanged
Park the driver on a semaphore via the existing `irq_event_isr`->`sem_post` pattern
(polling would burn the unprivileged thread's quantum). NO kernel change. Wait on RX
complete (`PSR.RSIF`) -- RX-done implies TX shifted, so one wait covers full-duplex.
The only device-specific need is W1C the SSC flag (`PSCR`) before the re-arm (auto on the
next `irq_wait`; `irq_ack` is now an optional early re-arm), else the line re-asserts on
unmask and storms (same hazard as the timer flag). Confirm the
SR-line->NVIC number against the interrupt-node table (as the console did for
USIC0_SR0_IRQ=84). FIFO/LIMIT burst mode is a later optimization; start single-word.

## API (faithful to "write a main, that's it")
- App-facing blocking: `int spi_transfer(void* tx, void* rx, size_t len)` -- enqueues
  to the driver thread, blocks on a semaphore. The app never touches MMIO/grants/IRQ.
- Driver thread: the spawned unprivileged owner of the MMIO grant + IRQ handle,
  started by a small privileged bring-up shim (does the boot config + issues the
  grant). Stack via `KOS_STACK_DEFINE` (pow2-aligned for PMSA).

## Open questions -- gated on the GPIO/timer first driver
1. Fault-vs-grant + decode: an ungranted MMIO access must be reported not escalated;
   a device-region violation may be a BusFault (not MemManage) -- confirm the reporter
   decodes both + prints the address.
2. DEV attr on the real transfer path (back-to-back TBUF/RBUF/PSR), not just an LED
   toggle, under enforcement on silicon.
3. Hardware CS needs nothing beyond the channel window (assumed yes).
4. SR-line -> NVIC number for the SSC indication interrupt.
5. Grant-time encodability rejection actually fires for a bad window on PMSA.

## Sequencing
stack-ownership refactor -> MMIO-grant mechanism (task #9) -> GPIO/timer driver on
XMC PMSA (validates the DEV path + fault-vs-grant + IRQ/W1C, answers the open
questions) -> THIS SPI driver. SPI is a pure addition: no kernel code, one pow2
region, CS folded in.
