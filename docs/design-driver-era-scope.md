<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The driver era -- scope / gap analysis

> **Status: ACTIVE.** The M4 gap list, reduced to the gaps still OPEN and the decisions that bound
> them. Section 4 records the ORDERING decision (driver era first, then SMP, then MMU), so any
> "M4 = SMP" framing inside it is the old numbering being argued against, not a live claim.
> `design/README.md` cites section 4 by number and eight design documents cite the `G<n>` labels, so
> neither is renumbered. For an exact contract go to `docs/reference/`; what is recorded here is what
> is not yet settled.

Framing (user's words). **M3 = POC**: endpoints/IPC, console handover, clock-select and the
fault-funnel reclaim are PROVEN, but each on ONE or TWO chips. **The driver era = the real deal**:
reuse those mechanisms across the FLEET and fix whatever gaps XMC-only (plus some K64F) testing hid.

---

## 1. What M3 landed, and where it is still a fallback

Each mechanism's exact contract is `reference/architecture.md`, `console.md`, `invariants.md` and
`porting.md`. All the gap list needs from that surface is the per-chip fan-out.

- **Endpoint/IPC** (syscalls 26/27/28): arch-independent, no per-chip gap. K64F + XMC 39/39 under
  enforcement, rest build-only.
- **Console handover**: this entry recorded userspace drivers on two chips
  (`system/driver/xmc4800/xmcuart`, `system/driver/mk64f/k64uart`). The set has since grown to ten
  driver directories, polled and IRQ-driven UARTs plus two USB CDC consoles; it is whatever
  `grep -rln KOS_SVC_CONSOLE system/driver/` reports, counted per driver directory rather than per
  file. G1 survives only for the boards the 2.1 table below still marks GAP.
- **Panic reclaim**: most of the fleet now carries a real body. The set is whatever
  `grep -rln '^void arch_console_reclaim(void)' arch/*/chip/` lists; every chip NOT in it falls back
  to `arch/common/arch_console_reclaim_default.cc`, which is a SILENT reclaim failure, so a
  driver-garbled UART eats the panic banner on those chips only (G2). The porting invariant
  forbidding that is `invariants.md`, `panic-console-probe-independent`.
- **Clock-select** (`arch_cpu_clock_set`, #30): XMC full (144/48), K64F staged (120/20.97), every
  other chip returns 0 from the fallback TU (G4). **Retune coherence** SPLITS: `arch_console_retune`
  matches it (XMC and K64F, no-op elsewhere), while `arch_console_flush_sync` has since grown a real
  body on ten chips (`grep -rln '^void arch_console_flush_sync(void)' arch/*/chip/`).

---

## 2. GAP LIST (the driver-era work)

Effort scale: S = a day-ish, M = a few days, L = a week+ / needs a design gate.
Silicon-gating: **HW** = needs the board on a bench; **NOW** = doable in-tree / QEMU.

### G1. Fleet-wide userspace UART / console drivers (OPEN)
Section 2.1 is the live per-board status, and the boards it still marks GAP are what is left. Each
driver = {claim the DEV window
via the MMIO grant, poll TX-ready, drive the ASC/UART, answer the stdout endpoint}, and `xmcuart` is
the template. Effort **M per chip family**, less within a family (the 3-4 STM32 USART parts share one
driver).

Order DECIDED by where the handover is a real security boundary, since that is what the grant model
is for: the CPU-side-MPU boards first (RX72M, ESP32-C6, each gated on its G5 prereq), then the coarse
and no-isolation parts (STM32 family, RP2040) where the driver still buys functional handover and the
polled-family reference. Rejected: easiest-first, which spends the era re-proving the mechanism on
boards that cannot enforce it.

### G2. Per-chip `arch_console_reclaim` (MOSTLY CLOSED)
Every board that ENABLES the handover needs a body or it violates the porting invariant. The contract
is `invariants.md` (`panic-console-probe-independent`) and `console.md`. The bodies that exist are
whatever `grep -rln '^void arch_console_reclaim(void)' arch/*/chip/` lists; run it against the chip
list under `arch/*/chip/` and the difference is what is still open. Read
`arch/arm/chip/xmc4800/usic_uart.cc` and `arch/arm/chip/mk64f/chip_mk64f.cc` as the worked examples.

The chips still on the fallback are QEMU semihosting parts with no console peripheral to reclaim
(`mps2`, `virt`, `nrf51`), the retired `sam3x8e`, and the two STM32 USART parts (`stm32f103`,
`stm32f302`), which are also the only chips section 2.1 marks GAP that have no console driver of any
kind. So G2 is no longer a fleet-wide gap: it closes when the STM32 driver lands, and its body is the
only register homework left.

**The homework each body is**: enumerate the registers a hostile or buggy driver can set to cause
TRUE SILENT LOSS, i.e. those whose reset default is benign, so rewriting the registers init sets does
not cover them. A module clock gate left off (XMC `KSCFG.MODEN`) silently drops every later store; a
flow-control enable with no wired counterpart (K64F `MODEM.TXCTSE`) parks the polled writer forever.
Every chip has that pair in its own spelling. For the one port still owing a body:

- **STM32 USART**: `CR1` (UE/TE, and LOOP), `CR3` (CTSE the silent-loss twin, HDSEL half-duplex),
  `BRR` re-derive from PCLK, `CR2` (LINEN/CLKEN synchronous).

The equivalents the shipped bodies already cover, kept because they are what the STM32 list is
patterned on: RX72M SCI `SCR`/`SMR`/`BRR`/`SPMR` (SS/CTSE is the `TXCTSE` twin)/`SEMR`; ESP32-C6 and
ESP32 LX6 UART `CONF0` (tx flow-control / loopback / txd_inv), `CLKDIV` re-derive, `CLK_CONF` gate,
RS485 and AT-command modes off, TXFIFO reset; RP2040 PL011 `UARTCR` (CTSEN/RTSEN/LBE loopback),
`UARTLCR_H`, `UARTIBRD`/`UARTFBRD` re-derive, `UARTDMACR` off.

Effort **S-M** for the remaining port; the register list is the work.

### G3. Handover validation per board (PARTLY CLOSED)
Functional handover wherever a driver exists; ISOLATION only where the MPU permits.
- **K64F functional half CLOSED** (frdmk64f, 2026-07-30): the full service list (`k64uart` +
  `k64dspi`) reported `[k64uart] driver up (polled TX)` and ran the whole 66-case TAP suite through
  the driver. Its reclaim body is written and still UNWITNESSED, the open remainder here.
- **Real per-thread peripheral isolation** (grant = a security boundary), OPEN on RX72M and ESP32-C6:
  both G5 prereqs (the RX IRQ demux, the C6 APM open) are CLOSED, and what is still missing is the
  on-silicon isolation witness. XMC is proven (xmcspi).
- **Coarse-AIPS (K64F)**: the grant is DOCUMENTATION, not enforcement, because SYSMPU does not gate
  peripherals and the AIPS bridge is all-user once a slot is opened (`reference/architecture.md`, the
  peripheral-MMIO matrix). **No-MPU (STM32F103, ESP32-LX6, nRF51)**: functional handover only;
  document it. QEMU-only (mps2/virt/microbit) is semihosting with no peripheral, so N/A.

Effort **S per board** (a scramble-then-panic test like XMC's `conreclaim`), mostly **HW**. Why
`conreclaim` is a standalone app rather than an option on the console demo is in
`design-m3-console-handover-stageii.md`.

### G4. Clock-select fleet-wide (OPEN; XMC full + K64F staged only, rest fallback-0)
Extend `arch_cpu_clock_set` per chip, or explicitly keep the fallback. Discipline (from
`design-m3-clock-select.md`): flash wait-states and voltage go UP *before* frequency rises and DOWN
*after* it falls; bracket the exact PLL/divider write; re-anchor the monotonic clock; re-derive baud;
re-arm the timer.

DECISION: opt-in per chip, and **declining is a legitimate outcome, not a gap to force**. Per-chip
feasibility already scoped. STM32F411, DEFERRED but feasible as a fixed set (park on HSI, PLL off,
rewrite N/P, relock). RP2040, feasible for `clk_sys`; the TIMER (`clk_ref`) is immune but the CONSOLE
is not, since `clk_peri` tracks `clk_sys`, so baud must be re-derived. Everything else (sam3x8e,
nrf51, f103/f302, mps2, esp32, C6, riscv) keeps fallback-0 until someone needs it.

Rejected: forcing a body onto every chip, which buys nothing on a board with one operating point and
adds an untestable privileged sequence per port. Effort **M per chip that opts in**, **S** to leave
the fallback and document it. Mostly **HW**. This is the MECHANISM seam only; POLICY is the
power-manager service (G7, sections 3 and 6).

### G5. Peripheral-isolation prereqs (PARTLY CLOSED)
- **ESP32-C6 APM/PMS global open: CLOSED.** `arch_init` (`chip_esp32c6.cc`, `apm_open_ree0`) programs
  the REE0 background permit at boot on every board, so it is no longer per-app. That bus-side unit
  is independent of PMP and defaults DENY-USER on peripheral targets, so a C6 userspace peripheral
  driver needs BOTH the per-thread PMP grant and the one-time global open; the register-level model
  is `reference/architecture.md` (the peripheral-MMIO matrix). An APM denial does NOT trap the way a
  PMP violation does, so a bad region hangs instead of faulting.
- **RX real-peripheral-IRQ demux: CLOSED, and this entry named a symbol that never shipped.**
  `kickos_rx_dev_pending_line` exists in no source file in the tree; the single-line hook was
  replaced by `kickos_rx_dev_dispatch` (`arch/rx/rxv3/arch_rxv3.cc`,
  `arch/rx/chip/rx72m/chip_rx72m.cc`), which is what the shipping `rxsci` driver dispatches
  through, and the GROUPBL0 group table it needed is in the chip
  (`arch/rx/chip/rx72m/irq.h`, `arch/rx/chip/rx72m/regs/icu.h`).
  `design-m4.6-irq-driver.md` already recorded the replacement; this page did not.
  The reason the doc gate did not catch the dead name is worth keeping: its identifier oracle
  matches UPPERCASE-prefixed names only, so a lowercase function or seam name cited in prose is
  outside the checked corpus entirely. IRQ ownership by an unprivileged driver is designed in
  `design-m4.6-irq-driver.md`.
- **m2-review-followups: OPEN (read).** Sweep `docs/m2-review-followups.md` for residual gaps before
  building drivers on top. Effort **S**, **NOW**.

### G6. Driver-API maturation (OPEN: the build-layering question)
The driver-lib + demo split is under way, not finished. `system/driver/<chip>/{xmcuart, xmcssc,
k64uart, k64dspi}` are LIBS (`add_library`) with a demo app linking them, while
`user/apps/<board>/{xmcspi, f411spi, k64drv, rxdrv}` are still monolithic DIAGNOSTIC apps
(`kickos_add_diagnostic_app`) with driver and demo fused.

**The open question is build layering, not packaging.** A chip driver lib lives under `system/`
because it is board support a consumer links on top of the OS rather than an app, even though it
builds UNPRIVILEGED like an app; the `system/` vs `user/` line is drawn by who links a thing, not by
privilege. That leaves one directory sharing a privilege posture with `user/` and a consumption model
with the kernel, and nothing enforces which side a new driver lands on. Rejected: moving the driver
libs under `user/`, which would make an out-of-tree consumer link board support out of the app tree;
and gating on privilege, which discriminates nothing now that root itself is unprivileged. Effort
**L** (design gate first).

How a driver is PACKAGED is decided in `design-m4-driver-model.md` (the class/service duality: the
driver-lib class as the primitive, the shared service composed on it 1:1, the two capability shapes,
the bus/device split). What a driver CONTRACT looks like is section 3.

### G7. Driver-era enabler services (init CLOSED, power-manager OPEN)
- **Init service: CLOSED.** The entry seam and a default init ship
  (`system/include/kickos/sys/init.h`, `system/init/`): `kickos_init_entry`, a `KICKOS_INIT_PROVIDER`
  target knob, and a default body that walks the board's service list before the app's
  `kickos_app_main`. It was settled EARLY on purpose, because the entry rename is a consumer-facing
  breaking change that is cheap now and expensive later, and because it is what spawns
  drivers-with-caps in dependency order (3.1).
- **Power-manager / clock-tree service: OPEN.** The userspace owner of the whole clock tree (PLL,
  dividers, central refcounted tree-gates, a rate-change-notifier fan-out), with the kernel keeping
  only the re-anchor and the privileged-step residue. DECISION: it comes AFTER the clock MECHANISM
  (G4) and after the first drivers, because it is the policy layer over G4 and the rate-change
  fan-out only matters once multiple derived-clock consumers exist. Rejected: building the standing
  service inside M4; section 6 cuts the M4 scope to the cascade-free parts and defers the live DVFS
  cascade to be built against the console as its first forced instance. Effort **L**, design gate.
  **Known contradiction, OPEN**: 3.1 makes CLOCK-TREE a persistent RUNTIME service brought up BEFORE
  the drivers, which cannot hold together with this dependency order.
  `design-m4-fable-review.md` finding 6 carries both halves and the cheaper resolution (the DAG's
  real dependency is only "gate the driver's clocks at bring-up", a one-shot init step like pinmux);
  `TODO.md` tracks it.

### G8. Gaps XMC-only testing HID (OPEN)
Per-chip hazards one-chip testing could not surface. Each is a prediction until that chip's driver
runs.
- **Flush-to-shift-idle differs per UART.** Reclaim and deinit must wait for the SHIFT register to
  drain, not just the holding register, or the last bits truncate. XMC USIC, K64F single-reg, PL011
  FIFO and ESP FIFO all differ. Relatedly, **FIFO UARTs** (PL011, ESP, some SCI) need a FIFO flush on
  reclaim (the K64F `CFIFO` precedent) and single-datum ones do not, a class the XMC test never
  covered.
- **Baud re-derivation.** XMC bakes a fixed 72 MHz constant, K64F re-derives from live
  `SystemCoreClock`. Under G4 EVERY driver must re-derive, a path an XMC-only test never exercised.
- **CTS/CTSE is the recurring silent-loss trap** (K64F `MODEM.TXCTSE`, STM32 `CR3.CTSE`, RX `SPMR`,
  PL011 `UARTCR.CTSEN`), which is why G2's register set is per-chip.
- **RX and ESP TX paths are unproven for handover**: SCI and ESP UART TX-idle plus FIFO semantics
  under a userspace driver. The RX IRQ-demux body it used to also wait on has landed (G5).
- **Line-idle transient on reclaim.** XMC's documented spurious leading byte comes from pinning TX
  low past a frame boundary, and whether it appears depends on each chip's passive-level handling, so
  each reclaim needs the same known-artifact honesty check.

### 2.1 Fleet UART-driver gap table (silicon-available FIRST)
Console peripheral per board. "ring" = kernel IRQ-drained today; driver = userspace UART driver
status.

| Board | Console UART (instance) | Isolation ceiling | Kernel console today | Userspace driver | Priority |
|---|---|---|---|---|---|
| XMC4800 | XMC USIC0-ch0 (U0C0) ASC @0x40030000 | PMSA per-thread (REAL) | ring + sync | **DONE (xmcuart)** | -- |
| K64F | Kinetis UART0 @0x4006A000 | coarse-AIPS (doc only) | ring + sync | **DONE (k64uart)**, reclaim unwitnessed | -- |
| RX72M | Renesas SCI6 @0x0008A0C0 | RX-MPU per-thread (REAL) | ring | **DONE (`rxsci`)** | -- (G5 IRQ demux closed with it) |
| ESP32-C6 | C6 UART0 @0x60000000 (128-FIFO) | PMP per-thread (REAL) | ring | **DONE (`c6uart`)** | -- |
| ESP32-WROOM (LX6) | Xtensa UART0 @0x3FF40000 (128-FIFO) | none (no MPU) | ring | **DONE (`lx6uart`)** | -- |
| STM32F411 (disco/blackpill) | USART2 @0x40004400 (old SR/DR), PA2 | PMSA (build-only HW) | ring | **DONE (`f4uartirq`)** | -- |
| STM32F302 (nucleo) | USART2 @0x40004400 (NEW ISR/TDR), VCP | PMSA (RAM-tight) | ring | GAP | 4 (STM32 NEW-model variant) |
| STM32F103 (bluepill-c8) | USART1 @0x40013800 (old SR/DR), PA9 | none | ring | GAP | 4 (shares the F411 old-model driver) |
| RP2040 (picopi) | ARM PL011 UART0 @0x40034000 (FIFO) | v6-M PMSA per-thread | ring | GAP for UART; a USB CDC console (`rpusb`) exists, `select`-only | 3 (PL011 reference; SMP board) |
| SAM3X (due) | SAM3X UART @0x400E0800 | none | RETIRED | skip | -- (unit retired, HW fault) |
| imxrt1062 (teensy41) | NXP LPUART6 @0x40198000 (FIFO) | PMSAv7 per-thread (REAL) | ring + sync | GAP for UART; a USB CDC console (`rt1062usb`) exists, `select`-only | -- (on the bench; `STATE.md` witness ledger) |
| rp2350 (pizero2350) | ARM PL011 UART1 @0x40078000 | PMSAv8 per-thread (REAL) | ring + sync | GAP for UART; a USB CDC console (`rpusb`) exists, `select`-only | -- (on the bench; `STATE.md` witness ledger) |
| mps2 / virt / microbit | semihosting (no peripheral) | QEMU | polled (semihosting) | N/A | -- |

`select`-only: no board names that service list as its default, so the image runs the kernel-owned
console unless configured with `-DKICKOS_SERVICE_LIST=<provider>` (`tests/static/service_lists.txt`).

STM32 driver note: the family splits into TWO register models, **old SR/DR** (F411 USART2, F103
USART1) and **NEW ISR/TDR** (F302 USART2). One STM32 driver with a compile or runtime model select
covers both; do NOT assume one register layout across the family. Exactly the class of gap an
XMC-only test could never surface.

---

## 3. DRIVER-FRAMEWORK DEPTH

### 3.1 Bring-up dependency DAG (the foundational services peripheral drivers stand on)
Peripheral drivers are not independent: they sit on shared central authorities that must be up first.
The init service (G7) brings them up in this order.

```
   INIT service (root, all authority). ONE-SHOT at bring-up, from a board pin-map:
     gate clocks -> mux pins -> grant MMIO caps at spawn -> spawn drivers -> app
        |                                   |
        v                                   v
   CLOCK-TREE service                 kernel clock residue
   RUNTIME: PLL, dividers,            (re-anchor only)
   central gates, DVFS +
   rate-change notify
        |
        v
   PINMUX step, inside init, NOT a service: assign each driver's pin functions from the
   board pin-map, THEN grant the pin's register window at spawn.
        |          |          |
        v          v          v
     UART       I2C        SPI      needs {clock}; pins pre-muxed; a mode-2 CS also needs
                                    a granted pin window (3.5). No GPIO service on the path.
```

Three distinct foundational SHAPES, matched to how often each CHANGES at runtime.
- **CLOCK-TREE, a RUNTIME service** (persistent). Owner of the shared PLL, dividers and central
  gates. A rate change cascades to every derived-clock consumer (UART re-derives baud, SPI its
  prescaler) through a Common-Clock-Framework-shape notifier. Central and refcounted, so a branch
  feeding two peripherals gates off only when BOTH are idle. Kernel residue is re-anchoring its own
  clock. It changes at runtime, so it must be a standing service. (This framing is what G7 flags as
  contradicting its own dependency order.)
- **GPIO, NOT a service but a DIRECT-MMIO grant** (3.5 carries the decision, the latency numbers and
  what stays deferred). Pin TOGGLING is direct MMIO, never a syscall, so the kernel touches GPIO only
  for the one-shot PINMUX and a driver needing a pin gets that pin's window granted AT SPAWN under a
  per-chip isolation ceiling. This REVERSES the earlier "GPIO service mints per-pin caps" model.
- **PINMUX, ONE-SHOT init-time config, NOT a service.** Pin-function assignment is set once at
  bring-up and does not change at runtime, unlike a hot GPIO CS or the clock tree under DVFS, so it
  needs no persistent service and COLLAPSES into init's bring-up sequence. Init muxes through
  `arch_pinmux_set`: the mux registers live in the shared SCU/PORT block alongside the clock gates,
  so the write lands privileged inside the kernel while init itself stays unprivileged, gated on
  `AUTH_PINMUX`. The seam, its per-chip backends and the declining fallback are
  `reference/porting.md` (*Pin-function config*). Caveat: a rare dynamic pin RE-config (runtime
  repurpose, or reconfiguring pins for low-power sleep) would be a COLD call back through the same
  seam if ever needed, never the common path.

Consequence: init is a topological bring-up (clock, mux, grant each driver its windows at spawn, the
byte/transfer drivers, apps), doing the one-shot pinmux itself, with no GPIO service anywhere on the
path. That is why init is a GATING enabler for the driver era rather than a nicety.

**The bring-up inputs are CONSUMER DATA, not tree literals** (spike `design-m4-oot-board-config.md`,
mechanism landed M4.4). kickos supplies MECHANISM; the board or product supplies POLICY as two POD
tables plus a `main`: `kos_board_pinmap` (`{port, pin, func}` routing) and `kos_service_list` (ordered
`kos_service_bringup {start, cfg}` entries the default init walks BEFORE `main`). Per-instance
parameters (register base, grant window, priority, target hz, CS choice, I2C address) travel as DATA
in `kos_service_cfg` rather than baked into the driver TU, so the SAME driver-class target serves N
instances by config alone (the LPUART1..8 and N-SPI case). Each list and pinmap definition is one
strong symbol chosen by a CMake target knob (`KICKOS_SERVICE_LIST` / `KICKOS_BOARD_PINMAP`), fail-loud
on a missing or misspelled target: no runtime manifest and no silent fallback, which is the anti-CapDL
tenet. **Per-BOARD today** (the in-tree `frdmk64f` and `xmc4800-relax` configs are REFERENCE
EXAMPLES); the per-app / fleet rollout is future work.

### 3.2 Driver API taxonomy by I/O model
The classical driver shapes map onto TWO IPC patterns.

| Driver | I/O model | IPC pattern | Kernel primitive |
|---|---|---|---|
| UART (console) | ASYNC byte-stream rx/tx | endpoint rendezvous + IRQ-as-event | CAP_ENDPOINT (LANDED) + tier-1 IRQ event |
| SPI | SYNC full-duplex transfer(tx,rx,len) | **CALL / REPLY** transaction | CAP_ENDPOINT + call/reply (**LANDED**: CAP_REPLY, `KOS_SYS_CALL`/`REPLY` 34/35) |
| I2C | SYNC addressed start/addr/rw/stop | **CALL / REPLY** transaction | same substrate; the wire CONTRACT lands, the driver body follows a bench target |

**The driver era surfaced the call/reply IPC requirement, which M4.4 then satisfied**, and the
taxonomy is why: the console is an async stream that the M3 rendezvous already served, while SPI and
I2C are request/reply TRANSACTIONS needing the L4-style fastpath the M3 endpoint spike had DEFERRED.
Contracts: `reference/ipc-call-reply.md` (transport) and `reference/bus-service.md` (the SPI/I2C
wire); the why is `book/synchronous-call-and-reply.md`.

Does {async-stream rendezvous} + {call/reply} cover uart + spi + i2c? UART yes, with the tx side a
stream and the rx side IRQ-as-event feeding the endpoint. SPI and I2C yes, over CAP_ENDPOINT plus the
MMIO grant, with I2C adding only addressing and start/stop framing in the segment list, and the first
I2C body waiting on a bench target device. Small transfers ride the kernel-copied bounded payload
inline (~212 B under `KOS_EP_MSG_MAX`); larger ones want a granted shared buffer, which the wire ABI
already reserves (`region_cap`/`offset`, DEFERRED) so it lands without an ABI break, on the same
physical-addressing discipline QW-3 asks of the IPC ring. So **the call/reply layer on CAP_ENDPOINT
is the first driver-framework primitive**, the shared substrate for every synchronous driver (SPI,
I2C, later block and net).

### 3.3 Multi-instance threading (the "4 SPI" question)
A chip with N SPI peripherals admits two shapes.
- **Thread-per-instance** (LEAN): N driver threads, each with its OWN SPI MMIO grant and its own
  request endpoint. One window, one thread, one boundary, so a fault in SPI2's driver cannot touch
  SPI1. Natural on the CPU-side-MPU boards. Cost: N threads (a stack each) and N endpoints.
- **One-driver + worker-pool** (SHARED): one driver thread owns all N windows, a worker pool fans out
  transactions. Sharing windows means WEAKER isolation, since one grant spans multiple peripherals
  and a bug in one path can scribble another's window. Saves threads. Only justified when a SINGLE
  instance needs concurrent pipelined transactions behind one window.

DECIDED: **thread-per-instance** as the default, because it is the honest expression of the
grant-as-security-boundary model and it is exactly the per-thread peripheral isolation the fleet
proved (xmcspi, rxdrv, c6blink). Add worker THREADS only within one instance's driver when that
peripheral needs concurrent in-flight transactions. Do NOT collapse multiple peripherals behind one
worker pool, which trades away the isolation the MPU gives.

### 3.4 DMA, the hard isolation problem (distinct sub-topic, FLAG)
DMA engines write PHYSICAL addresses and BYPASS the MPU, since these MCUs have no IOMMU or SMMU. A
userspace driver programming a DMA channel could point it at KERNEL memory or another domain's, a
full isolation hole: the MPU protects CPU accesses only, and the DMA master is a separate bus master
it never sees. Second axis: **a DMA controller is a SHARED resource** (channels feeding many
peripherals) like the clock tree and pinmux, so it wants a CENTRAL owner allocating channels and
validating descriptors rather than a per-driver grant of the whole block. Options.
- **Kernel-mediated DMA setup**: a syscall validates the descriptor's src/dst against the driver's
  granted regions before arming the channel. The driver never writes the DMA address registers
  (privileged, outside the grant window) and instead asks the kernel to program a descriptor it has
  proven safe. This is the clock-tree "privileged-step residue" pattern, the dangerous write staying
  kernel-side behind a seam. Cost: a per-transfer syscall, fine for setup and bad for high-rate
  scatter-gather.
- **Defer DMA**: ship polled and IRQ-driven drivers first. The current SPI/UART drivers are already
  polled or IRQ (`k64dspi` blocks on the EOQ IRQ), so nothing needs DMA yet and the driver era keeps
  moving without opening the hole.

Verdict: **defer DMA to a dedicated sub-topic**, drivers polled or IRQ first. When it lands,
kernel-mediated descriptor validation plus a central channel allocator is the shape. A distinct HARD
problem, not part of the first driver-framework cut.

### 3.5 GPIO, a direct-MMIO grant rather than a kernel service
**DECIDED (spike `design-m4-gpio-direct-spike.md`).** GPIO is NOT a kernel service and not a pin
allocator that mints caps. The kernel touches GPIO only for the ONE-SHOT privileged PINMUX at init
(3.1); pin TOGGLING is DIRECT MMIO. A driver needing a pin gets that pin's register block granted at
spawn (the task #9 grant-at-spawn MMIO path) and writes it itself, with a per-chip isolation ceiling
on the granted window. A kernel pin allocator plus toggle syscall (`GPIO_CLAIM`/`WRITE`/`READ`) WAS
built (af7d99a) and then REMOVED once the latency spike below showed the syscall path cannot serve a
hot pin. This reverses the earlier "GPIO service mints per-pin caps" model.

**Why direct, and not a syscall.** The hot case is an SPI chip-select toggled every transaction. A
`GPIO_WRITE` SVC round trip is ~100-200 cycles (exception entry, decode, `IrqLock`, `cap_resolve`,
one store, exception return; none of it elidable, and the cap resolve IS the validation that
justifies a syscall), so ~0.7-1.7 us per toggle at fleet clocks. A mode-2 CS brackets the transfer
with two toggles, while a 16-bit frame at 72 MHz SCLK is only 222 ns and a 16-byte transfer 1.78 us.
Two `GPIO_WRITE` SVCs therefore add 1.4-3.4 us SERIALIZED into a transaction whose entire payload is
smaller than that, 7-16x slower for the 16-bit frame, and inject `IrqLock` spans into the
highest-rate path in the system. Rule of thumb: **kernel-mediated GPIO is fine at <= ~1 kHz and NEVER
inside a bus transaction; direct MMIO for everything hotter.** Rejected: forcing the whole fleet
through the slow path to paper over one chip's inability to isolate a toggle register from its mux.
KickOS accepts per-chip isolation ceilings instead (the K64F coarse-AIPS precedent, section 2.1).

**SPI chip-select has TWO first-class modes**, selected per DEVICE and never per chip: mode 1 is the
engine-native hardware PCS, preferred wherever the device is wired to a HW-PCS pin and tolerates the
engine's CS behavior; mode 2 is a driver-owned direct GPIO CS, required when the device needs a
coherent CS level held across a multi-byte transaction the engine cannot sustain, or when the CS net
has no HW-PCS pin. Both are shipped policies with their per-controller mechanics in
`reference/bus-service.md` (*Chip-select policy*). What matters HERE is that mode 2 is load-bearing
(it is the production KickCAT CS path on K64F), so a hot toggle must be reachable directly on every
chip, which is what the ceiling table below answers.

**Per-chip isolation ceiling for a direct toggle window.** Can the atomic set/clear plus input
registers be granted as a narrow window that EXCLUDES the pin-mux and all shared authority?

| chip | mux-free toggle window carvable? | enforcement floor (GPIO data) | hot GPIO-CS (mode 2) direct? |
|---|---|---|---|
| **XMC4800** (PMSAv7) | **NO** -- OMR (+0x04) and IOCR0-12 (+0x10-0x1C) share one 32 B min-region/subregion; no bit-band at 0x48028000. Read-only IN window IS carvable | 32 B min; smallest honest grant = 64 B whole-port window INCLUDING that port's mux | **YES** via dedicated port (board layout makes the over-grant harmless) or trusted over-grant; never via kernel toggle |
| **K64F** (SYSMPU+AIPS) | MOOT -- GPIO block is a crossbar slave with no PACR / no SYSMPU coverage | **NONE** for GPIO data+direction (every unpriv thread reaches every pin); mux stays supervisor via AIPS | **YES, silicon-proven** (k64dspi PSOR/PCOR per transaction); no grant needed |
| **RX72M** (RX-MPU) | **YES, mux-free**: PODR page [0x0008C020,+0x10) excludes PDR/PMR/MPC | 16 B pages, byte-exact, 8 regions. Residue: 16 ports' output data per page. Peripheral enforcement UNVERIFIED on silicon | **YES** -- direct grant + BSET/BCLR single-instruction discipline (PODR is RMW, no set/clear alias) |
| **ESP32-C6** (PMP) | **YES, best in fleet**: 8 B NAPOT over W1TS/W1TC (+0x08); IO_MUX + in-block FUNCn_OUT_SEL excluded | 4 B granularity, 16 entries (backend uses 8). Residue: bank-wide data bitmask, zero mux authority. Small-NAPOT-over-peripheral needs one silicon check | **YES** -- direct grant (atomic W1TS/W1TC) |
| **ESP32-WROOM** (LX6, no MPU) | MOOT -- no MPU, no privilege split | **NONE** (trust-only chip) | **YES trivially** (atomic W1TS/W1TC; nothing to isolate) |

ALLOCATION exclusivity is chip-INDEPENDENT bookkeeping; REGISTER-grant exclusivity is a
chip-DEPENDENT floor, the isolation-ceiling pattern of section 2.1 applied at pin granularity. The
two chips that cannot draw a per-pin line (XMC by PMSAv7 subregion math, K64F by having no peripheral
gate at all) are hardware ceilings of a class KickOS already documents, not an argument for a
kernel-mediated fleet default. Verdict: direct-MMIO mode-2 CS is viable on every chip, the kernel
toggle nowhere hot.

**Still DEFERRED, all four verified unbuilt.**
- **N MMIO windows per creation.** `thread_create_call` carries exactly ONE window (`attr.mmio_base`,
  `user/include/kickos/sys/abi.h`), while an XMC SPI driver with a mode-2 CS needs TWO (USIC channel
  plus port) and a C6 driver needs its peripheral window plus the W1TS/W1TC entry. Additive: a small
  bounded N, each admissibility-checked and region/PMP-budget-checked as today, no new object model.
  Zero windows needed on K64F or WROOM (open floor).
- **The vendor-neutral `kos_gpio` helper.** `kos_gpio_claim_out` (cold arbitration plus mux verify)
  returning a descriptor `{mode, set_addr, clr_addr, in_addr, mask}` with the per-chip styles folded
  in, so `kos_gpio_set/clear/get` inline to one store or load (BSET/BCLR on RX) and the SAME driver
  logic runs on all five chips. `kos_gpio_require_direct` fails LOUD at bring-up so a hot-CS driver
  never runs degraded on a chip that can only offer the kernel path.
- **Runtime mint-and-delegate** of a per-pin window to an already-running holder. NO forcing consumer
  exists, since every M4 CS is known at bring-up and served by grant-at-spawn, and it is a kernel
  object-model change (generation, revoke-vs-running-holder, region-budget-at-mint) that must not be
  smuggled in through GPIO. Deferred until a dynamic-allocation consumer on a carvable chip exists.
- **Shared-IRQ demux and any userspace GPIO service.** Cold path only, landing with its first real
  IRQ-consuming consumer, orthogonal to the toggle path: a shared GPIO IRQ line hardware-forces a
  demux so IPC there is acceptable, whereas a CS toggle never is. Designed in
  `design-m4.6-irq-driver.md`.

Cross-ref pinmux (3.1) and DMA (3.4): same shared-resource-vs-performance-vs-isolation tension,
different hot/cold profile.

---

## 4. THE MILESTONE-NUMBERING QUESTION (primary deliverable)

> **DECIDED 2026-07-20, Option A.** Driver era = **M4**, SMP = **M5**, MMU / new-platform = **M6**;
> `roadmap.md` is authoritative. The numbers have since moved again -- the driver era took M4 and
> M5, SMP is **M6** and MMU / new-platform is **M7** -- and the rest of this section is written in
> the new numbering. The ORDERING decided here is unchanged; only the labels moved.
> Work is still named by THEME where the number is not load-bearing,
> because the roadmap's own "anytime-coherence" tagging means several pieces are not strictly gated
> by number. `design/README.md` cites this section by number.

### 4.1 The three remaining big rocks
1. **DRIVER ERA (M4)**, single-core: fleet UART/console drivers, per-chip reclaim, clock-select
   fleet-wide, the driver framework (call/reply IPC, taxonomy, multi-instance), and the enabling
   services init, clock-tree/power-manager, pinmux, gpio.
2. **SMP (M6)**: one kernel image across cores (RP2040/RP2350), which reworks the foundation because
   `IrqLock` ("IRQs off means exclusive") is single-core-only (`design-m7-smp.md`).
3. **MMU / new-platform (M7)**: x86_64 PC plus i.MX8MP heterogeneous AMP, MMU KickOS on the A53 and
   MPU KickOS on the M7 over cross-core IPC (`design-mmu-era-exploration.md`).

**QW-3 carries M6.** Keep the shared-IPC ring contract PHYSICALLY addressed from day one
(`design-mmu-era-exploration.md:330`). It was flagged for "M3/M4" but belongs with the SMP/AMP
cross-core IPC work.

### 4.2 The dependency argument (retained as the rationale)
The driver era does NOT depend on SMP: entirely single-core, sharing no invariant refactor, and the
console and clock seams it uses are already single-core correct. SMP is an OPTIMISATION until there
is something to run, since "run the workload at 2x" presupposes the workload exists. The IPC order is
right this way, because call/reply and the SMP cross-core ring are the SAME IPC lineage (control-plane
sync plus data-plane shared-memory async), so call/reply first informs the ring and QW-3 is honoured
in both. The i.MX8MP endgame needs the driver era AND AMP/IPC AND the MMU, so it is strictly last
regardless; x86_64 is mostly independent MMU work (MMU plus a new boot/interrupt model) and can slot
wherever the MMU work is scheduled.

### 4.3 The rejected orderings
- **Option B, SMP first** (the old roadmap M4). It honored the then-current roadmap, de-risked the
  multicore foundation while the codebase was small, and rode RP2350 M33 momentum. Rejected because
  it builds a foundation with little to run on it, designs the cross-core ring BEFORE call/reply
  (backwards), and "driver era as anytime-coherence in parallel" is exactly how the driver era had
  already been treated, which is what left the fleet at one console driver.
- **Option C, interleave.** Driver-era CORE first (fleet drivers, reclaim, clock-select, init), THEN
  SMP with call/reply plus QW-3 as its IPC front-half, THEN framework maturation and the clock-tree
  service. The honest compromise had RP2350-M33 hardware momentum been the stronger pull. Rejected
  because it splits the driver era across two numbers, separating the framework work from the drivers
  that motivate it.

---

## 5. PRIORITY / SEQUENCING (what unblocks what)

Within the driver era. The init-service entry seam and the call/reply layer are landed, so they are
prerequisites rather than steps.
1. **Foundational services** (3.1): pinmux (landed), clock gating at bring-up, the GPIO direct-grant
   path. Mix of **NOW** (design/sim) and **HW** (validate).
2. **Fleet console drivers + reclaim** (G1/G2/G3), silicon-available first: RX72M and ESP32-C6, each
   gated on its G5 prereq, then the STM32 family and RP2040 (PL011). Per-chip **HW**.
3. **Clock-select fleet** (G4) per opt-in chip, **HW**; then the **power-manager / clock-tree
   service** (G7) as the policy layer on top, **HW + design**.
4. **Driver framework maturation** (G6): the remaining lib/demo splits and the build-layering call.
   **NOW** (design + sim), validated on **HW**.
5. **DMA** (3.4): DEFERRED sub-topic, polled and IRQ drivers first.

Silicon-gated: G1 per-chip drivers, G2 reclaim validation, G3 handover validation, G4 clock-select,
and all HW re-validation (G5's rx72m IRQ-demux body was on this list and is CLOSED). Doable
NOW: the G6 design work and lib/demo split, the m2-review-followups read, and every
register-homework enumeration in G2.

### 5.1 Prereq / blocker summary
Each blocker is stated with its gap: the RX72M IRQ dispatch seam (G5, since CLOSED by
`kickos_rx_dev_dispatch`), every fleet console driver on that chip's reclaim body (G2),
ordered bring-up on init and the foundational steps (3.1, G7), and the clock-tree SERVICE on the
first drivers existing, even though the clock MECHANISM (G4) can precede them (G7's contradiction).

---

## 6. M4 design decisions (fable review 2026-07-20 + review discussion)

The adversarial review is `design-m4-fable-review.md`, which doubles as the driver era's risk register
and carries each finding's OUTCOME. These are the accept/revise calls that supersede earlier prose
above where they differ.

- **Clock: there IS an M4 service, scoped to the SAFE parts, and only the live cascade defers.** The
  G7 power-manager decision, in three parts. (1) A clock ORACLE, so a driver can query its PARENT or
  BRANCH clock (a UART needs its fPERIPH for baud, SPI its prescaler); `kos_cpu_clock_hz()` gives
  only the CORE clock, so this is a real read-only cascade-free need AND it is the seam a later
  rate-change notify walks. (2) One-shot BOOT tree config/select/gate per branch, pinmux-shaped
  privileged pokes behind the kernel seam. (3) The live DVFS rate-change CASCADE (cross-domain
  notify, quiesce, re-derive: a two-phase commit across untrusted drivers) is DEFERRED and will be
  built against the CONSOLE as the first forced notify instance, since M3 today merely REFUSES a
  retune while the console is USER_OWNED (`clock_select.cc:32`, verified correct) and that
  refuse-path is where the handshake grows. Authority is a SYSCALL-GATING capability and never an
  SCU/RCC MMIO grant, because such a window ungates any peripheral and can kill the kernel timer
  clock; the kernel keeps every shared-clock-block register, so a service bug is restartable policy
  rather than a flash or PLL hard fault. This fixes the roadmap's "Later/clock-tree" prose.
- **GPIO: direct-MMIO grant, kernel does only pinmux** (finding 9, superseded by
  `design-m4-gpio-direct-spike.md`; the decision, its numbers and the XMC negative result are 3.5).
  Pinmux stays a one-shot privileged init step, verified rather than set by the cold claim path. A
  shared-port IRQ demux, if ever needed, is a non-blocking per-subscription notification with
  sticky-pending and an ISFR ack before delivery, NEVER a parked rendezvous send that would let one
  slow subscriber deafen the port.
- **Call/reply carries the scheduling contract (finding 4): CLOSED.** Without priority donation,
  synchronous SPI/I2C over CAP_ENDPOINT is unbounded inversion on every transaction, with KickCAT
  cyclic traffic as the victim. Direct-handoff donation on call is in the shipped contract.
- **The transfer ABI is offset-based from day one (finding 10, the one M7 landmine).** A
  large-transfer request speaks `{region-cap, offset, len}` and never a raw pointer: cheap now, an
  ABI break at M6 when a domain becomes a page-table root. This pulls the QW-3 DISCIPLINE, not the
  ring implementation, into the call/reply contract.
- **Timed / abortable IPC -> EARLY-M4: CLOSED.** The primitive ships: `KOS_SYS_SEND_TIMED`,
  `KOS_SYS_RECV_TIMED` and `KOS_SYS_CALL_TIMED` (`user/include/kickos/sys/abi.h`), dispatched in
  `kernel/syscall/syscall.cc`, with the park unwind in `kernel/thread/park.cc`
  (`endpoint_wait_abort`). **The two items this entry named as blocked on it are no longer blocked
  by it**: the clock-cascade quiesce-timeout (the deferred part 3 of the clock entry above) and the
  driver-death waiter wake (the entry below) each now stand on their own remaining work rather than
  on a missing primitive. Their status is otherwise unchanged. What is still absent is the wider
  object `design-m4.6-irq-driver.md` section 7.5 names, receive-from-either-of-two-sources, which
  stays an M6 kernel object.
- **Driver crash/restart plus resource reclaim: OPEN.** Pin caps, clock-gate refcounts, AIPS slots and
  endpoint holders all leak on driver death; only the panic-path console reclaim exists. `TODO.md`
  carries the `kos_cap_narrow` endpoint-rights gap that blocks a real driver-death story.
- **RP2350 M33 arch question: SETTLED as "armv7m board plus a v8-M MPU backend"**, with no `armv8m`
  arch directory. PMSAv8 (RBAR+RLAR+MAIR) is a different unit from PMSAv7 (RBAR+RASR) and gets its own
  backend (`design-rp2350-mpu-armv8m.md`); the core is a v7-M superset and needed no split.

---

## 7. API-CONFORMANCE OBJECTIVE (the neutrality matrix)

The gap list above is the WORK; this section pins WHY M4 does it. The objective is not "ship drivers"
for its own sake: full driver support is the VEHICLE that validates the M4 service APIs against real
hardware variation. The end deliverable is a set of console/UART, gpio, pinmux, clock/power and bus
(SPI/I2C) service APIs proven GENUINELY VENDOR-NEUTRAL rather than accidentally shaped around one
vendor's register model, clock tree or pin scheme. **The falsifier**: a driver that lands cleanly on
one vendor and then forces an API change to land on the next has proven the API leaked a vendor
assumption. The matrix is how that leak gets caught.

The bus API's first neutrality check runs on two of the four matrix boards at once, and it is
PARTLY passed. XMC4800 (USIC-SSC, hardware MSLS/SELO0 CS via `PCR.FEM`) and FRDM-K64F (DSPI,
driver-owned GPIO CS via direct `PSOR`/`PCOR`) run the SAME wire and the SAME class,
`<kickos/driver/spi.h>`, so the controller register model stayed engine-internal and the wire
names none of it (`reference/bus-service.md`). What did NOT come out neutral is `cs_policy`: the
two engines accept DISJOINT subsets of it (`KOS_BUS_CS_HW` on the XMC, `KOS_BUS_CS_GPIO` on the
K64F, each refusing the other with `-KOS_ENOTSUP`), so a consumer that moves between them must
change that one field. That is the leak the matrix exists to catch, recorded rather than papered
over. The RX72M (RXv3/RX-MPU) and ESP32-C6 (PMP) legs, and the I2C driver body, extend the check
across the remaining axes.

### 7.1 The four-board neutrality matrix
Four easy-to-flash boards, chosen for DIVERSITY across vendor AND arch/MPU family at once.

| Board | Arch | MPU family | Vendor |
|---|---|---|---|
| c6 (ESP32-C6) | rv32imac | PMP | Espressif |
| xmc (XMC4800) | armv7m | PMSAv7 | Infineon |
| k64f (FRDM-K64F) | armv7m | SYSMPU | NXP |
| rx72m | RXv3 | RX-MPU | Renesas |

Four vendors by four arch/MPU families is a far stronger neutrality test than adding another ARM
board. Two boards can share an ISA (xmc and k64f are both armv7m) yet still exercise DIFFERENT MPU
units (PMSAv7 against SYSMPU) and different vendors, and the two non-ARM boards stress the API where
an ARM-only fleet is silent. A vendor bias baked into a service API cannot survive all four ports.

### 7.2 The driver->board mapping principle (the scope guard)
"Total / full driver support" is an ASPIRATION to FULL per-board peripheral coverage, NOT a set
bounded a-priori to the peripheral CLASSES the M4 services define (console/UART, gpio, pinmux,
clock/power, plus at least one bus SPI and/or I2C). Reaching for a board's WHOLE complement is how we
discover hardware that needs a dedicated or new API, and that discovery IS the point. The bound is a
COMPLEXITY-vs-GAIN weighting per candidate, NOT a class restriction: the per-board prioritized
backlog and the cross-board neutrality shortlist in `docs/design-m4-driver-matrix.md` are what keep
scope from ballooning into a four-vendor BSP while still exercising each API, and what surface the
high-gain NON-class peripherals (analog-in, PWM, the C6-ETM/XMC-ERU/RX-ELC event fabric, the
xmc/rx72m on-chip EtherCAT SC) a class restriction would hide. That document is the source-of-record
for the unlanded-peripheral survey, and `roadmap.md`'s scope-guard bullet rests on it too.

So the discipline is to prefer MAPPING different drivers to different boards to maximize variation
coverage, over duplicating one driver across all four. A service API is "neutrality-proven" once its
class has a working driver on boards spanning the vendor/arch spread, not once every board carries
every driver. Console/UART is the exception that DOES want all four (7.3); the rest earn their proof
by covering the diversity axes rather than by uniform replication.

### 7.3 The no-probe constraint (a design INPUT, not an afterthought)
Two of the four matrix boards, rx72m and c6, are easy to FLASH but have NO usable debug probe.
Bring-up there is PRINT-DEBUG ONLY: no single-step, no register peek over a probe, only what the
board can print. A hard input to the service design rather than a testing footnote, because it means
the services must be CONSOLE-OBSERVABLE to be brought up on those two boards at all. A service that
can only be diagnosed through a debugger cannot be brought up on half the neutrality matrix.

Consequences.
- Console/UART is the FIRST driver on every matrix board, not just the interesting-isolation ones,
  because it is the observability substrate every other bring-up on rx72m and c6 depends on. This is
  why console/UART is the one class that wants all four boards (7.2).
- The M4 services should surface bring-up state (init progress, allocation and grant results,
  clock/baud derivation, transfer status) over the console, so a print-only board can diagnose a
  failed handover, mux or clock-select without a probe.
- The porting invariant of section 1 and G2 (a real `arch_console_reclaim`) matters MORE here: on a
  no-probe board a garbled UART that eats the panic banner leaves NO diagnostic channel at all.
