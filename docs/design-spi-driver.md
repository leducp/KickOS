<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design brief: unprivileged userspace SPI driver

> **Status: LANDED** -- `xmcspi` shipped as `user/apps/xmc4800-relax/xmcspi/` and is
> silicon-proven (2026-07-17): the canonical per-thread PMSA MMIO-isolation result, where a
> granted 512-byte USIC DEV window does an internal SSC loopback (4 words, `tx == rx`) AND an
> ungranted SCU poke faults MemManage (`CFSR=0x82`). Note the milestone numbers in this brief
> predate the M4/M5/M6 renumbering (`design-driver-era-scope.md` sec.4). See `design/README.md`
> for the taxonomy.

Decision record. The board-level account, the console captures and the seam allowlist are
`reference/boards.md`; the SPI service wire contract is `reference/bus-service.md`. The open
questions this brief carried were all answered by the 2026-07-17 silicon run.

TARGET NOTE: the SPI driver KickCAT needs is K64F/DSPI, and that counterpart is
`design-spi-driver-k64f-dspi.md`. It records the silicon-established ceiling this brief was
blocked on: K64F peripheral isolation is AIPS-PACR-based, not SYSMPU, so a DSPI window grant is a
genuine per-thread capability only on the CPU-side-MPU flavors (XMC PMSA, RISC-V PMP), not on K64F.

Builds on `design-task9-mmio-driver.md` (privileged-only MMIO grant; MMIO as a Domain region
`R|W|DEV` no-X; Option-A grant-at-spawn) and reuses the two-tier IRQ path unchanged.

## Chip: XMC4800, USIC in SSC (SPI) mode
DECIDED: lead on XMC/USIC-SSC. PMSA `DEV` attr is enforcement-proven on this silicon and the USIC
block is already modeled clean-room (SSC is a protocol layer mirroring the ASC/UART layer, reusing
the clock/baud/mux/FIFO/IRQ ops). REJECTED at the time: leading on K64F/DSPI, whose SYSMPU
peripheral-bridge gating under user mode was then unproven. Do not lead a driver on an unproven
isolation backend.

## Organizing principle
The granted USIC-channel MMIO window IS the security boundary, not the semantics of the registers
inside it. Everything in-window affects only THIS channel on its ALREADY-assigned pins (baud,
frame, mode, CS), which is contained, not an escalation. The operations that reach OTHER pins or
peripherals stay privileged and OUT of the window: the port pin-mux (IOCR) and the SCU clock gate.

## Privilege split
- Privileged boot (once): SCU clock ungate and de-reset; baud/prescaler; controller mode and frame
  (`CCFG.SSC`, `CCR.MODE=SSC`, `SCTR`, `PCR` master + CS policy); pin-mux (IOCR for
  SCLK/MOSI/MISO/CS plus `DXnCR` input select). The pin-mux is programmed LAST, to avoid an
  idle-level glitch. Then the channel window is handed to the driver via the grant.
- Unprivileged driver (per transfer): load TX (`TBUF`/FIFO), read RX (`RBUF`), start, wait
  completion, W1C the SSC flag (`PSCR`), assert and deassert hardware CS (`PCR` SELO). All
  in-window.

Escalation surfaces kept privileged and out-of-window: SCU (the system clock tree, which could
ungate any peripheral) and port IOCR (pin-mux, which could steer SPI onto arbitrary pins or capture
another owner's pins).

## The MMIO grant: ONE channel window
- Grant = one USIC channel base, size **0x200 (512 B)**, `R|W|DEV` no-X. PMSA-clean under
  reject-not-round: 512 is pow2 and over the 32 B minimum, and every channel base is 0x200-aligned,
  so it is one descriptor with no pad and no split.
- Minimal window: NOT the whole USIC, NOT the peripheral bridge, NOT IOCR or SCU.
- The console owns U0C0, so SPI takes a different channel (U0C1). The exact channel and pins are a
  board-header detail, pinned against the Relax Kit board manual rather than guessed.

### Chip-select is the key SPI-specific choice
- **Hardware CS (SELOx) recommended.** CS enable and timing are in-window
  (`PCR.MSLSEN`/`SELO`/`SELINV`), so it costs zero extra regions.
- **GPIO CS rejected on XMC:** it needs the port data register (port+0x00/0x04) but IOCR sits at
  port+0x10, and PMSA's 32-byte minimum region from the port base spans 0x00-0x1F, so the window
  would expose IOCR and hand over pin-remux escalation. A data-register window cannot be split from
  the mux at 32 B granularity here. Hardware CS sidesteps it.

Region budget (ARMv7-M PMSA, 8 max): code 1 + appdata 1 + stack 1 + SPI window 1 = **4/8**, with CS
folded in. Cheaper than the task #9 GPIO+timer example (5).

## Transfer model: IRQ-driven, tier-1 reused unchanged
DECIDED: park the driver on a semaphore via the existing `irq_event_isr` -> `sem_post` pattern, with
no kernel change. REJECTED: polling, which burns the unprivileged thread's quantum.

Wait on RX complete: RX-done implies TX shifted, so one wait covers full-duplex. The one
device-specific need is W1C of the SSC flag (`PSCR`) before the re-arm, else the line re-asserts on
unmask and storms (the same hazard as the timer flag). Two RM facts the driver depends on:

- A single-word frame is SOF=1, so the completion flag is **AIF** (alternative receive), NOT RIF
  (XMC4800 RM 18.4.2.7). The driver arms and W1Cs both.
- Loopback is INTERNAL (RM 18.2.3.5 Loop Back Mode: the DX0 input stage selects on-chip input "G",
  the channel's own transmitter), so no port pins are muxed and no MISO/MOSI jumper is needed.

FIFO/LIMIT burst mode is a later optimization; start single-word.

## API (faithful to "write a main, that's it")
- App-facing blocking call: `int spi_transfer(void* tx, void* rx, size_t len)`, which enqueues to
  the driver thread and blocks on a semaphore. The app never touches MMIO, grants or IRQs.
- The driver thread is the spawned unprivileged owner of the MMIO grant and the IRQ handle, started
  by a small privileged bring-up shim that does the boot config and issues the grant. Its stack
  comes from `KOS_STACK_DEFINE` (pow2-aligned for PMSA).
