<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design brief: K64F/DSPI unprivileged userspace SPI driver

> **Status: LANDED** -- `k64dspi` shipped and is silicon-proven (2026-07-17): a 4-word SOUT->SIN
> loopback with `tx == rx` over the AIPS-opened slot, and it reached OPERATIONAL against a real
> LAN9252. It is exported as the `kickos_k64dspi` lib (`system/driver/mk64f/k64dspi`). Kept as the
> DECISION record; the register-level facts it used to carry -- the full DSPI register map with
> reset values, the clock gate, the NVIC number and the AIPS `PACR` computation -- now live in
> `reference/bus-service.md` (K64F DSPI0) and `reference/architecture.md` (the PACR derivation,
> under Memory domains), both code-synced. Where this brief and the shipped driver differ,
> `bus-service.md` names the differences and the CODE wins.

The SPI driver KickCAT actually needs: a DSPI master on the FRDM-K64F driving an EtherCAT Slave
Controller over its SPI PDI. Counterpart of `design-spi-driver.md` (XMC/USIC-SSC). Register facts
were derived clean-room from the K64 Sub-Family RM Rev.4, Oct 2019. Instance: **SPI0 (DSPI0)**, the
Arduino R3 header SPI and the natural landing for an ESC shield.

## The hardware ceiling this brief designs WITHIN (read first)

K64F peripheral isolation is **AIPS-PACR-based, NOT SYSMPU** -- silicon-proven via the `k64drv`
PIT driver. The SYSMPU is a bus-slave-side unit: it guards flash and SRAM crossbar ports and NEVER
sees a peripheral-bridge access, so it cannot gate the DSPI at all. The AIPS bridge gates by
privilege level and bus master, per 4 KiB slot, and each slot's `PACR` `SP` field resets to
supervisor-only.

Consequence, stated honestly: once the shim clears the DSPI slot's `PACR SP` bit, the DSPI
registers are reachable by **every** unprivileged thread in the system, at whole-4-KiB-slot
granularity. The driver is isolated from the kernel and from other domains' MEMORY (SYSMPU still
enforces SRAM domains), but there is **no per-thread peripheral boundary** on K64F. On XMC PMSA or
RISC-V PMP the identical MMIO grant IS a genuine per-thread capability; here it is not, and no
kernel code recovers that. The decision that follows from it is decision 1.

## Decisions

1. **Keep the microkernel invariant -- drivers in userspace -- and ACCEPT the coarser K64F
   ceiling, documented.** The alternative was mediating every DSPI access through a syscall, which
   would buy per-thread peripheral isolation on this one board at the cost of the property the whole
   design exists for. Rejected. The grant is still ISSUED: it keeps the driver's region set coherent
   and it is exactly what a PMSA or PMP sibling port enforces, so the same spawn signature ports --
   but on THIS silicon it is documentation, not enforcement.

2. **The boundary K64F actually draws is kernel-vs-user, per SLOT, decided by whether the shim
   opens it.** Not per-thread, and not per-register-window. Say so rather than implying the window
   is the boundary.

3. **The escalation surfaces stay privileged and out of the driver's reach regardless**: the SIM
   clock gates (could ungate any peripheral) and the PORTD pin mux (could re-mux SPI onto or off
   arbitrary pins). The driver gets the DSPI register window and nothing else.

4. **Mux the pins LAST, after the controller is at a defined idle.** SCK and SOUT are outputs, so
   programming CPOL before the mux takes the pins is what avoids an idle-edge glitch on the ESC
   clock line. The XMC brief makes the same "mux last" point; it is a fleet rule, not a K64F one.

5. **Configure while HALTED, then release.** `MCR` boots the module disabled and halted, so the
   shim clears `MDIS`, flushes both FIFOs, sets `PCSIS` for a CS-idle-high target, and only then
   drops `HALT`. Exact reset values: `bus-service.md`.

6. **`EOQ` pacing, not `TCF`.** `TCF` fires per frame -- N interrupts for an N-frame ESC command --
   while marking the last frame's `PUSHR.EOQ` and waiting `SR.EOQF` fires ONCE per logical transfer
   regardless of frame count. So the wake model does not change when a FIFO-burst optimisation
   lands: push N, EOQ the last, one wake. The side effect is intended: setting `EOQF` auto-clears
   `SR.TXRXS` (the module stops at end-of-queue), the driver clears `EOQF`, and the next `PUSHR`
   restarts the queue -- which is a request/response ESC exchange exactly.

7. **W1C the status flag BEFORE the line is re-armed.** An uncleared level re-asserts on unmask and
   STORMS the line -- the exact PIT `TIF` hazard `k64drv` hit. The re-arm is the loop's next
   `kos_irq_wait` (auto-re-arm); an explicit `kos_irq_ack` is an OPTIONAL early re-arm for the
   compute-then-wait shape. Either way, W1C comes first.

8. **Hardware `PCS` was the recommendation; the shipped driver uses a GPIO CS.** The brief chose HW
   `PCS0` because it folds CS entirely into the one DSPI window -- zero extra regions, zero GPIO --
   and rejected a GPIO CS for needing a second granted window and, worse, re-introducing the PORT
   block whose mux is the escalation surface. What changed it: a DSPI HW-PCS `CONT` window clocks a
   trailing dummy byte on release, so HW CS suits only per-frame-CS-tolerant devices, and a coherent
   multi-phase transaction needs a software CS. The shipped driver drives PTC4 via `GPIOC`
   `PSOR`/`PCOR`, which costs no grant because K64F GPIO is an ungated crossbar slave -- so the
   objection that decided against it does not actually apply on this chip. See `bus-service.md`,
   *Chip-select policy*.

9. **The app-facing API stays faithful to "write a main, that's it."** The app never touches MMIO,
   grants or IRQs; it calls a blocking transfer. KickCAT sits on top: the driver provides the
   transport, and the ESC PDI framing (register addressing, command bytes, wait states) is entirely
   KickCAT's concern. The generalised wire form of that API is `reference/bus-service.md`.

Region budget: code + appdata + stack + the DSPI window = **4 of ~12** SYSMPU RGDs. The DSPI RGD is
inert for peripheral isolation here but is still spent, for region-set coherence and portability.

## Open questions / risks

1. **No per-thread peripheral isolation on K64F** -- the ceiling, not a bug. If KickCAT ever needs
   driver-A-vs-B DSPI isolation on THIS board, the only route is syscall mediation, which decision 1
   ruled out of scope.
2. **Fault-vs-grant decode**: an ungranted or still-supervisor-only DSPI access surfaces as a
   **BusFault** (an AIPS error response), NOT a SYSMPU MemManage, so the fault reporter must decode
   the bus-fault path and print the address. That obligation is now a porting contract
   (`reference/porting.md`, the fault-reporter contract).
3. **ESC SPI mode (CPOL/CPHA) and baud** are ESC-part-specific boot constants -- pin them to the
   chosen ESC datasheet (ET1100 and LAN9252 differ), never guessed.
4. **FRDM header J-pin numbers** could not be confirmed from the extractable UG text (the pinout is
   an image). The PORT/pin/ALT2 mapping itself IS RM-confirmed; the J2 pin positions were confirmed
   at the bench.
5. **`EOQ` STOPPED-state re-arm**: that W1C `EOQF` plus the next `PUSHR` cleanly restarts the queue
   for back-to-back exchanges was a silicon item.
6. **Full-duplex length > FIFO depth**: the TX FIFO is 4 entries on SPI0. A single-word model is
   immune; a burst optimisation must respect that depth (TFFF/RFDF pacing).
