<!-- SPDX-License-Identifier: CECILL-C -->
# The driver era -- scope / gap analysis

**EXPLORATORY -- NOT A CONTRACT.** A scoping lens over the work that turns the M3
mechanisms (proven on XMC / K64F only) into a real, fleet-wide capability. Ordering is now
DECIDED (Option A: driver era = **M4**, SMP = **M5**, MMU = **M6**; see section 4), but this
doc still names work by THEME -- "the driver era" -- where the number is not load-bearing. No
implementation here; a checklist lands in `TODO.md` and the design in `docs/reference/` only
after the per-item gate.

Framing (user's words): **M3 = POC** -- endpoints/IPC, console handover, clock-select, the
fault-funnel reclaim all PROVEN, but each on ONE or TWO chips. **The driver era = the real
deal** -- reuse the M3 mechanisms and make them real ACROSS THE FLEET, fixing whatever gaps
XMC-only (+ some K64F) testing hid. The console handover was never tried fleet-wide.

---

## 0. Outline

1. State of the M3 mechanisms (what landed, on which silicon)
2. Gap list -- the driver-era work, per item, effort + dependency + silicon-gating
3. Driver-framework depth -- the bring-up DAG, the API taxonomy, multi-instance, DMA, GPIO
4. The milestone-numbering question -- driver-era vs SMP vs MMU, ordering options + rec
5. Priority / sequencing -- what unblocks what; silicon-gated vs doable-now
6. M4 design decisions (fable review)
7. API-conformance objective -- the four-board neutrality matrix + the driver->board mapping

---

## 1. What M3 actually landed (the POC surface)

| Mechanism | Kernel seam | Real backend bodies | Weak default | Silicon proof |
|---|---|---|---|---|
| Endpoint/IPC (CAP_ENDPOINT) | syscalls 26/27/28, `SlotPool<Endpoint>`, `wq_block`/`wq_pop_highest` | arch-independent | n/a | K64F + XMC 39/39 under enforcement; rest build-only |
| Console handover | `ConsoleState`, `kos_console_publish` (#29), stdout cap @idx 0 | userspace driver = **XMC only** (`user/driver/xmcuart`) | drop chip path | XMC end-to-end app->IPC->driver->wire, under enforcement |
| Panic reclaim | `arch_console_reclaim`, `kickos_isr_fault`->`kpanic_enter` funnel | **XMC (USIC) + K64F (UART0) only** | weak no-op (`console.cc:288`) -- SILENT reclaim failure | XMC scramble-then-panic PASS; K64F built, silicon-pending (no K64F console driver) |
| Clock-select | `arch_cpu_clock_set` (#30) + re-anchor/baud/timer tail | **XMC full + K64F staged only** | weak return 0 (`clock_select.cc:77`) | XMC 144/48 + K64F 120/20.97 |
| Retune console coherence | `arch_console_flush_sync`, `arch_console_retune` | XMC, K64F | weak no-op (`console.cc:296-297`) | folds into the above |

The **fault-funnel porting invariant** (`docs/reference/porting.md`, `invariants.md`
`panic-console-probe-independent`): any board that hands its console to a userspace driver
MUST ship a real `arch_console_reclaim` body -- the weak no-op is a silent reclaim failure
(a driver-garbled UART then eats the panic banner). This invariant is the spine of the
fleet-wide reclaim gap below.

---

## 2. GAP LIST (the driver-era work)

Effort scale: S = a day-ish, M = a few days, L = a week+ / needs a design gate.
Silicon-gating: **HW** = needs the board on a bench; **NOW** = doable in-tree / QEMU.

### G1. Fleet-wide userspace UART / console drivers  (M3 did XMC ONLY)
`user/driver/xmcuart` is the sole userspace console driver. Every other board's console is
still kernel-owned; the handover mechanism exists but has no driver to hand to.

Per board (see the fleet UART table in section 2.1). Each userspace UART driver =
{claim the DEV window via MMIO grant, poll TX-ready, drive the ASC/UART, answer the stdout
endpoint}. `xmcuart` is the template. Effort **M per chip family**, less within a family
(the 3-4 STM32 USART parts share a driver).

Priority = do the chips where the handover is *interesting* first: the CPU-side-MPU boards
(XMC done, RX72M, ESP32-C6) where the grant is REAL per-thread isolation. K64F next (proves
the reclaim body already written). STM32/RP2040 are coarse/none-isolation -- driver still
worth it for functional handover + as the polled-family reference.

### G2. Per-chip `arch_console_reclaim`  (only XMC + K64F have a body)
Every board that ENABLES the handover needs a reclaim body, or it violates the porting
invariant. The contract (from the two existing bodies): **straight-line ABSOLUTE stores
only** (idempotent + re-entrant from a nested fault -- NO read-modify-write on a
driver-garbled value), rewrite every in-window writable register init sets, PLUS the
reset-default registers a hostile/buggy driver can set to cause **true silent loss**:

- XMC: `KSCFG.MODEN` FIRST (module clock gate -- off => every later store silently dropped),
  then CCR/TBCTR/RBCTR/baud/SCTR/TCSR/PCR/FMR(TDV clear)/PSCR/CCR-reenable-LAST.
- K64F: `MODEM.TXCTSE=0` (else the polled writer waits forever on an absent CTS -- the true
  silent-loss case), `C3.TXINV`, `S2`, `IR.IREN`, `C7816`, `PFIFO`, `CFIFO` flush, re-derive
  baud from live `SystemCoreClock`, `C2=TE` last.

The per-chip homework = enumerate that chip's silent-loss registers. Gaps + watch-registers:
- RX72M SCI: SCR (TE/RE/clock-src), SMR, BRR re-derive, SPMR (CTS/RTS enable -- SS/CTSE the
  silent-loss twin of K64F TXCTSE), SEMR (baud-rate-gen mode/MDDR), FCR if the FIFO SCI.
- ESP32-C6 UART: CONF0 (tx flow-ctrl / loopback / txd_inv), CLKDIV re-derive, CLK_CONF gate,
  RS485/AT-cmd modes off, TXFIFO reset.
- ESP32 LX6 UART: same UART IP family as C6 -- CONF0/CLKDIV/txd_inv, FIFO reset.
- STM32 USART: CR1 (UE/TE), CR3 (CTSE -- silent-loss twin, HDSEL half-duplex, LOOP via CR1),
  BRR re-derive from PCLK, CR2 (LINEN/CLKEN synchronous).
- RP2040 PL011: UARTCR (CTSEN/RTSEN/LBE loopback), UARTLCR_H (line ctrl / FIFO enable),
  UARTIBRD/UARTFBRD re-derive, UARTDMACR off.
- SAM3X UART/USART: retired unit -- skip unless a working board returns.
Effort **S-M per chip** once the driver exists (the register list is the work).

### G3. Handover validation per board  (K64F end-to-end NEVER run)
Functional handover everywhere a driver exists; ISOLATION only where the MPU permits.
- **Real per-thread peripheral isolation** (grant = a security boundary): XMC (proven,
  xmcspi), RX72M, ESP32-C6 (needs the APM open, G5). Here the handover is enforced.
- **Coarse-AIPS (K64F)**: SYSMPU does NOT gate peripherals; the AIPS bridge does (per
  privilege+master, per 4 KB slot, all-user once opened). So the K64F grant is
  DOCUMENTATION, not enforcement. Validate FUNCTIONAL handover + the reclaim body (already
  written, silicon-pending). K64F end-to-end was never run because there is no K64F console
  driver yet (G1).
- **No-MPU (STM32F103, ESP32-LX6, nRF51)**: handover is functional only; document it.
- QEMU-only (mps2/virt/microbit): semihosting console, no real peripheral -- N/A / skip.
Effort **S per board** (a scramble-then-panic test like the XMC one). Mostly **HW**.

### G4. Clock-select fleet-wide  (XMC full + K64F staged only; rest weak-0)
Extend `arch_cpu_clock_set` per chip, or explicitly keep the weak default. Discipline (from
`design-m3-clock-select.md`): flash wait-states + voltage go UP *before* frequency rises,
DOWN *after* it falls; bracket the exact PLL/divider write; re-anchor the monotonic clock;
re-derive baud; re-arm the timer. Per-chip feasibility already scoped:
- STM32F411 -- DEFERRED (feasible as a fixed set: park on HSI, PLL off, rewrite N/P, relock).
- RP2040 -- feasible for clk_sys; the TIMER (clk_ref) is immune but the CONSOLE (clk_peri
  tracks clk_sys) is NOT -- must re-derive baud.
- Everything else (sam3x8e, nrf51, f103/f302, mps2, esp32, C6, riscv) -- keep weak-0 until
  someone needs it. This is a legitimate "explicitly weak" outcome, not a gap to force.
Effort **M per chip that opts in**; **S** to leave weak-default + document. Mostly **HW**.
NOTE: this is the mechanism seam only; POLICY is the power-manager service (G7 / section 3).

### G5. Peripheral-isolation prereqs  (unblock userspace peripheral drivers per chip)
- **ESP32-C6 APM/PMS global open** -- a SECOND bus-side unit, independent of PMP, defaults
  DENY-USER on peripheral targets. A C6 userspace peripheral (UART/GPIO) driver needs BOTH
  the per-thread PMP grant AND a one-time global APM open. Scoped in `design-c6-driver.md`.
  **Blocks the C6 console driver + any C6 peripheral driver.** Effort **M**, **HW**.
- **RX real-peripheral-IRQ demux** -- `kickos_rx_default_irq` is still a stub; a real
  IRQ-driven RX driver (or the ring console generalisation) needs it. Effort **M**, **HW**.
- **m2-review-followups** -- sweep `docs/m2-review-followups.md` for residual gaps before
  building drivers on top. Effort **S**, **NOW** (read).

### G6. Driver-API maturation  (KickCAT POC -> a real reusable contract)
Today's evidence of "real apps on KickOS" is thin: KickCAT is the only consumer, one board,
a driver more demo than API (`kickcat_slave` is in the KickOS tree via
`kickos_add_application`). The **driver-app inconsistency** is the smell:
- `user/driver/xmcuart`, `user/driver/k64dspi` are LIBS (`add_library`) -- the right shape.
- `user/apps/{xmcspi,f411spi,k64drv,rxdrv}` are monolithic DIAGNOSTIC apps
  (`kickos_add_diagnostic_app`) -- driver + demo fused.
Maturation = the driver-lib + demo split (tasks #17/#18): each driver a reusable lib with a
typed contract; the demo an app that links it. What a real driver contract looks like is the
subject of section 3 (the API taxonomy). Effort **L** (design gate first). How a driver is
PACKAGED -- the class/service duality (driver-lib class as the primitive, the shared service
composed on top of it 1:1, the two capability shapes, the bus/device split, watchdog + sensor
cases) -- is decided in `design-m4-driver-model.md`.

### G7. Driver-era enabler services  (init + power-manager/clock-tree)  -- M4 or later?
From the roadmap notes. Assessment:
- **Init service** -- rename the entry (`kos_init_entry`) to separate init from the app +
  ship a default init that does configurable bring-up then calls user `main` with a cap set.
  The entry RENAME is a consumer-facing breaking change -- **settle it EARLY** (cheap now,
  breaks consumers later). VERDICT: **in the driver era, and early** -- it is the thing that
  spawns drivers-with-caps in dependency order (section 3.1), so it is a GATING enabler, not
  a nicety. Effort **M** for the seam, **L** for the full default service.
- **Power-manager / clock-tree service** -- the userspace owner of the whole clock tree
  (PLL, dividers, central refcounted tree-gates, a rate-change-notifier fan-out; kernel keeps
  only the re-anchor + privileged-step residue). VERDICT: **driver era but AFTER the clock
  MECHANISM (G4) and the first drivers** -- it is the policy layer over G4, and it needs the
  rate-change fan-out that only matters once multiple derived-clock consumers (drivers)
  exist. So: mechanism first, service later-in-era. Effort **L**, design gate.

### G8. Gaps XMC-only testing HID
Things the fleet rollout will surface that one-chip testing could not:
- **Flush-to-shift-idle differs per UART**: reclaim/deinit must wait for the shift register
  to drain, not just the holding register. XMC USIC vs K64F single-reg vs PL011 FIFO vs
  ESP32 FIFO all differ. A driver that returns before the last bit clocks out truncates.
- **Baud re-derivation**: XMC uses a fixed 72 MHz constant; K64F re-derives from live
  `SystemCoreClock`. Under clock-select (G4) EVERY chip's driver must re-derive, not bake a
  constant -- an XMC-only test never exercised the re-derive path on other chips.
- **Reclaim depth varies**: the silent-loss register set is per-chip (G2). CTS/CTSE is the
  recurring trap (K64F MODEM.TXCTSE, STM32 CR3.CTSE, RX SPMR, PL011 UARTCR.CTSEN).
- **RX / ESP TX paths untested for handover**: RX SCI and ESP UART TX-idle + FIFO semantics
  under a userspace driver are unproven; RX's IRQ demux is still a stub (G5).
- **FIFO vs single-datum**: FIFO UARTs (PL011, ESP, some SCI) need FIFO-flush on reclaim
  (K64F CFIFO precedent); single-datum ones (XMC-ASC-ish) do not -- a class the XMC test
  never covered.
- **Line-idle transient on reclaim** (XMC's documented spurious leading byte from pinning TX
  low past a frame boundary) -- may or may not appear per chip depending on the passive-level
  handling; each chip's reclaim needs the same "known artifact" honesty check.

### 2.1 Fleet UART-driver gap table  (silicon-available FIRST)
(Console peripheral per board; "ring" = kernel IRQ-drained today; driver = userspace UART
driver status.)

| Board | Console UART (instance) | Isolation ceiling | Kernel console today | Userspace driver | Priority |
|---|---|---|---|---|---|
| XMC4800 | XMC USIC0-ch0 (U0C0) ASC @0x40030000 | PMSA per-thread (REAL) | ring + sync | **DONE (xmcuart)** | -- |
| RX72M | Renesas SCI6 @0x0008A0C0 | RX-MPU per-thread (REAL) | ring | GAP | 1 (real isolation; needs G5 IRQ demux; TIE-prime HW-unverified) |
| ESP32-C6 | C6 UART0 @0x60000000 (128-FIFO) | PMP per-thread (REAL) | ring | GAP | 1 (real; needs G5 APM open) |
| K64F | Kinetis UART0 @0x4006A000 | coarse-AIPS (doc only) | ring + sync | GAP (reclaim body ready) | 2 (proves reclaim; end-to-end never run) |
| ESP32-WROOM (LX6) | Xtensa UART0 @0x3FF40000 (128-FIFO) | none (no MPU) | ring | GAP | 3 (functional only) |
| STM32F411 (disco/blackpill) | USART2 @0x40004400 (old SR/DR), PA2 | PMSA (build-only HW) | polled | GAP | 3 (STM32 old-model reference driver) |
| STM32F302 (nucleo) | USART2 @0x40004400 (NEW ISR/TDR), VCP | PMSA (RAM-tight) | polled | GAP | 4 (STM32 NEW-model variant) |
| STM32F103 (bluepill-c8) | USART1 @0x40013800 (old SR/DR), PA9 | none | polled | GAP | 4 (shares F411 old-model driver) |
| RP2040 (picopi) | ARM PL011 UART0 @0x40034000 (FIFO) | v6-M PMSA per-thread | polled | GAP | 3 (PL011 reference; SMP board) |
| SAM3X (due) | SAM3X UART @0x400E0800 | none | RETIRED | skip | -- (unit retired, HW fault) |
| imxrt1062 (teensy) | NXP LPUART6 @0x40198000 (FIFO) | MPU deferred | build-only | skip for now | -- (not in silicon fleet) |
| rp2350 | ARM PL011 UART0 @0x40070000 | PMSAv8 (deferred) | build-only | skip for now | -- (not flashed; SMP-era board) |
| mps2 / virt / microbit | semihosting (no peripheral) | QEMU | polled (semihosting) | N/A | -- |

STM32 driver note: the family splits into TWO register models -- **old SR/DR** (F411 USART2,
F103 USART1) vs **NEW ISR/TDR** (F302 USART2). One STM32 driver with a compile/runtime model
select covers both; do NOT assume one register layout across the family (a gap an XMC-only
test could never surface).

Silicon-available worth-doing set, in order: **RX72M, ESP32-C6, K64F, then the STM32 family
+ RP2040 (PL011).** The LX6 ESP32-WROOM is functional-only (no MPU). The rest are QEMU/retired
-- skip.

---

## 3. DRIVER-FRAMEWORK DEPTH

### 3.1 Bring-up dependency DAG (foundational services the peripheral drivers stand on)
Peripheral drivers are not independent -- they sit on shared, central authorities that must
be up first. The INIT service (G7) brings them up in this order:

```
        +---------------------------------------------+
        |  INIT service (privileged root)             |
        |  ONE-SHOT at bring-up, from a board pin-map: |
        |   * PINMUX -- privileged pin-function config |
        |     (NO runtime service; folds into init)    |
        |   * gate clocks, grant MMIO caps, spawn      |
        +---------------------------------------------+
                  |                        |
                  v                        v
         +----------------+       +----------------+
         |  CLOCK-TREE    |       | (kernel clock  |
         |  service       |       |  residue:      |
         |  RUNTIME: PLL, |       |  re-anchor)    |
         |  dividers,     |       +----------------+
         |  central gates,|
         |  DVFS + rate-  |
         |  change notify |
         +----------------+
                  |
                  v
         +----------------+   ONE-SHOT PINMUX (init does it, privileged): assign
         |  PINMUX step   |   each driver's pin functions from the board pin-map,
         | (in init, not  |   THEN grant the pin's register window at spawn. No
         |  a service)    |   GPIO service on the path -- the driver toggles the
         +----------------+   granted window DIRECTLY (see 3.5).
             |     |     |
             v     v     v
      +------+ needs {clock}   +------+ {clock}  +------+ {clock, a granted
      | UART | (pins pre-muxed)| I2C  |          | SPI  |  pin window for a
      +------+                 +------+          +------+   direct-GPIO CS}
```

Foundational shapes -- the peripheral drivers stand on THREE distinct kinds, matched to how
often each CHANGES at runtime:
- **CLOCK-TREE -- a RUNTIME service** (persistent). Owner of the shared PLL/dividers/central
  gates. A rate change (DVFS) cascades to every derived-clock consumer (UART re-derives baud,
  SPI its prescaler) via a Common-Clock-Framework-shape notifier. Central + refcounted (a
  branch feeding two peripherals gates off only when BOTH idle). Kernel residue = re-anchor its
  own clock. It CHANGES at runtime, so it must be a standing service.
- **GPIO -- NOT a service; a DIRECT-MMIO grant** (see 3.5). Pin TOGGLING is direct MMIO, never a
  syscall. A driver that needs a pin gets that pin's register window granted AT SPAWN (the grant-
  at-spawn MMIO path) and writes it itself, under a per-chip isolation ceiling on the window. The
  kernel touches GPIO only for the one-shot PINMUX at init (below). The runtime pin ALLOCATOR that
  mints-and-delegates per-pin caps, and the shared GPIO IRQ demux, are DEFERRED to M4.4 -- no
  driver forces them yet (the SPI-CS path is a direct-grant pin, not a minted cap). A kernel pin
  allocator + toggle syscall was built (af7d99a) and REMOVED after the 3.5 latency spike showed a
  syscall cannot serve a hot CS. (This reverses the earlier "GPIO service mints per-pin caps"
  model.)
- **PINMUX -- ONE-SHOT init-time config, NOT a service.** Pin-function assignment is set once at
  bring-up and does not change at runtime (unlike a hot GPIO CS, or the clock tree under DVFS),
  so it needs no persistent service: it COLLAPSES into the init service's bring-up sequence.
  Init muxes a driver's pins (privileged -- the mux registers live in the shared SCU/PORT block
  alongside the clock gates, natural since init is the privileged root that grants caps + spawns
  anyway), THEN spawns the driver, which never touches pinmux at runtime. Driven by a board
  pin-map. Caveat: rare dynamic pin RE-config (runtime repurpose, or reconfiguring pins for
  low-power sleep) would be a COLD privileged call if ever needed -- not the common path.

Consequence: the init service is a topological bring-up (clock -> mux the pins -> grant each
driver its pin windows at spawn -> the byte/transfer drivers -> apps), doing the one-shot pinmux
itself; there is no GPIO service on the path (pins toggle directly, 3.5). This is why the init
service is a GATING enabler for the driver era, not a nicety.

### 3.2 Driver API taxonomy by I/O model
The classical driver shapes map onto TWO IPC patterns:

| Driver | I/O model | IPC pattern | Kernel primitive |
|---|---|---|---|
| UART (console) | ASYNC byte-stream rx/tx | endpoint rendezvous + IRQ-as-event | CAP_ENDPOINT (LANDED) + tier-1 IRQ event |
| SPI | SYNC full-duplex transfer(tx,rx,len) | **CALL / REPLY** transaction | CAP_ENDPOINT + a call/reply layer (**deferred**) |
| I2C | SYNC addressed start/addr/rw/stop | **CALL / REPLY** transaction | same |

**KEY INSIGHT: the driver era surfaces the call/reply IPC requirement.** The console (async
stream) rides the synchronous *rendezvous* that already landed. But SPI/I2C are
request/reply TRANSACTIONS: a client sends a transfer request, BLOCKS, and gets the result
back -- which is exactly the "call/reply fastpath" the M3 endpoint spike DEFERRED (TODO
notes the sem_post token-handoff already drives an immediate switch, so the fastpath SHAPE
exists, but the reply-capability half is not built).

Analysis -- does {async-stream rendezvous (landed)} + {a call/reply layer} cover
uart+spi+i2c?
- UART: covered by the landed rendezvous + IRQ event. The tx side is a stream; the rx side
  is IRQ-as-event feeding the endpoint. No call/reply needed.
- SPI/I2C: need call/reply. The transfer contract on top of CAP_ENDPOINT + the MMIO grant:
  - client holds a cap to the driver's request endpoint; driver holds its SPI MMIO grant.
  - `transfer(tx_buf, rx_buf, len)` = client `kos_send`s a request {op, len, inline-or-
    shared tx bytes}, then blocks on the REPLY. The endpoint's kernel-copied bounded payload
    already carries small transfers inline; larger ones want a granted shared buffer (avoid a
    double copy) -- that shared-buffer path is the same physical-addressing discipline QW-3
    flags for the IPC ring.
  - driver does the MMIO transaction under its grant, `kos_send`s the reply {status, rx bytes}
  - the missing piece = a REPLY capability (a one-shot, auto-consumed cap back to the caller)
    so the driver replies to exactly the caller without a standing per-client endpoint. This
    is the L4 call/reply fastpath. It is the concrete driver-era ask on top of M3's endpoints.
- I2C = SPI's shape + addressing/start-stop framing in the request struct; same IPC.

Recommendation: **build the call/reply (reply-cap) layer on CAP_ENDPOINT as the first
driver-framework primitive** -- it is the shared substrate for every synchronous driver
(SPI, I2C, and later block/net). The async-stream half is already there for UART.

### 3.3 Multi-instance threading (the "4 SPI" question)
A chip with N SPI peripherals -- two shapes:
- **Thread-per-instance** (LEAN): N driver threads, each with its OWN SPI MMIO grant + its
  own request endpoint. Clean per-thread-peripheral isolation -- matches the grant model
  exactly (one window, one thread, one boundary). A fault in SPI2's driver cannot touch SPI1.
  Natural fit for the CPU-side-MPU boards. Cost: N threads (stack each), N endpoints.
- **One-driver + worker-pool** (SHARED): one driver thread owns all N windows, a worker pool
  fans out transactions. Shares windows => WEAKER isolation (one grant spanning multiple
  peripherals, or the driver holds all N -- a bug in one path can scribble another's window).
  Saves threads. Only justified when a SINGLE instance needs CONCURRENT transactions
  (pipelined) and you want a pool behind one window.

Lean: **thread-per-instance** as the default -- it is the honest expression of the grant =
security-boundary model, and the per-thread-peripheral isolation the fleet just proved
(xmcspi/rxdrv/c6blink) is exactly this. Add worker THREADS only within a single instance's
driver when that one peripheral needs concurrent in-flight transactions. Do NOT collapse
multiple peripherals behind one worker pool -- that trades away the isolation the MPU gives.

### 3.4 DMA -- the hard isolation problem (distinct sub-topic, FLAG)
DMA engines write PHYSICAL addresses and BYPASS the MPU (these MCUs have no IOMMU/SMMU). So
a userspace driver that programs a DMA channel could point it at KERNEL memory (or another
domain's) = a full isolation hole. The MPU protects CPU accesses only; the DMA master is a
separate bus master the MPU never sees.

Options:
- **Kernel-mediated DMA setup** -- a syscall that validates the DMA descriptor's src/dst
  addresses against the driver's granted regions before arming the channel. The driver never
  writes the DMA address registers directly (those stay privileged / outside the grant
  window); it asks the kernel to program a descriptor it has proven safe. Analogous to the
  clock-tree "privileged-step residue" pattern: the dangerous write stays kernel-side behind
  a seam. Cost: a per-transfer syscall (fine for setup, bad for high-rate scatter-gather).
- **Defer DMA** -- ship polled / IRQ-driven drivers first; solve the DMA isolation story
  later. The current SPI/UART drivers are already polled/IRQ (k64dspi blocks on EOQ IRQ), so
  nothing needs DMA yet. This keeps the driver era moving without opening the hole.

Second axis: **DMA controllers are a SHARED resource** (channels feeding many peripherals) --
like the clock tree and pinmux. So DMA also wants a CENTRAL owner (a DMA service that
allocates channels + validates descriptors), NOT a per-driver grant of the whole DMA block.

Verdict: **defer DMA to a dedicated sub-topic**; drivers are polled/IRQ first. When DMA
lands, kernel-mediated descriptor validation + a central channel allocator is the shape. Flag
this as a distinct HARD problem, not part of the first driver-framework cut.

### 3.5 GPIO -- direct-MMIO grant, not a kernel service
**DECIDED (spike `design-m4-gpio-direct-spike.md`).** GPIO is NOT a kernel service or a pin
allocator that mints caps. The kernel touches GPIO only for the ONE-SHOT privileged PINMUX at
init (3.1); pin TOGGLING is DIRECT MMIO. A driver that needs a pin gets that pin's register
block granted at spawn (the task #9 grant-at-spawn MMIO path) and writes it itself, with a
per-chip isolation ceiling on the granted window. A kernel pin allocator + toggle syscall
(`GPIO_CLAIM`/`WRITE`/`READ`) WAS built (af7d99a) and then REMOVED after the latency spike below
showed the syscall path cannot serve a hot pin. (This reverses the earlier "GPIO service mints
per-pin caps" model this section used to carry.)

**Why direct, not a syscall.** The hot case is an SPI chip-select toggled every transaction. A
`GPIO_WRITE` SVC round trip is ~100-200 cycles (exception entry + decode + `IrqLock` +
`cap_resolve` + one store + exception return -- none of it elidable, the cap resolve IS the
validation that justifies the syscall), so ~0.7-1.7 us per toggle at the fleet clocks. A mode-2
CS brackets the transfer with two toggles; a 16-bit frame at 72 MHz SCLK is only 222 ns, a
16-byte transfer 1.78 us. Two `GPIO_WRITE` SVCs add 1.4-3.4 us of overhead SERIALIZED into a
transaction whose entire payload is smaller than that -- 7-16x slower for the 16-bit frame -- and
inject `IrqLock` spans into the highest-rate path in the system. Rule of thumb: **kernel-mediated
GPIO is fine at <= ~1 kHz and NEVER inside a bus transaction; direct MMIO for everything hotter.**
Forcing the whole fleet through the slow path to paper over one chip's inability to isolate a
toggle register from its mux would be the wrong tradeoff -- KickOS accepts per-chip isolation
ceilings (the K64F coarse-AIPS precedent, 3.7 / section 2.1).

**SPI chip-select has TWO first-class modes**, selected per DEVICE by the SPI driver, never by
chip:
- **Mode 1 -- hardware PCS**: the engine-native chip select. Preferred and optimal wherever the
  device is wired to a HW-PCS pin and tolerates the engine's CS behavior (per-frame de-assert, or
  a CONT window over the whole transaction). Hardware-timed, zero software overhead.
- **Mode 2 -- driver-owned direct GPIO CS**: required when the device needs a coherent CS level
  held across a multi-byte transaction the engine cannot sustain (de-asserts per frame / breaks
  across FIFO refills -- the Stage-D DSPI bug where releasing HW PCS0 clocked a trailing dummy
  byte and corrupted a length-sensitive ESC mailbox write), or when the CS net has no HW-PCS pin.
  This is the production KickCAT CS path on K64F today (`user/driver/k64dspi`, PTC4 by PSOR/PCOR
  from the unprivileged driver thread).

**Per-chip isolation ceiling for a direct toggle window** -- can the atomic set/clear + input
registers be granted as a narrow window that EXCLUDES the pin-mux and all shared authority?

| chip | mux-free toggle window carvable? | enforcement floor (GPIO data) | hot GPIO-CS (mode 2) direct? |
|---|---|---|---|
| **XMC4800** (PMSAv7) | **NO** -- OMR (+0x04) and IOCR0-12 (+0x10-0x1C) share one 32 B min-region/subregion; no bit-band at 0x48028000. Read-only IN window IS carvable | 32 B min; smallest honest grant = 64 B whole-port window INCLUDING that port's mux | **YES** via dedicated port (board layout makes the over-grant harmless) or trusted over-grant; never via kernel toggle |
| **K64F** (SYSMPU+AIPS) | MOOT -- GPIO block is a crossbar slave with no PACR / no SYSMPU coverage | **NONE** for GPIO data+direction (every unpriv thread reaches every pin); mux stays supervisor via AIPS | **YES, silicon-proven** (k64dspi PSOR/PCOR per transaction); no grant needed |
| **RX72M** (RX-MPU) | **YES, mux-free**: PODR page [0x0008C020,+0x10) excludes PDR/PMR/MPC | 16 B pages, byte-exact, 8 regions. Residue: 16 ports' output data per page. Peripheral enforcement UNVERIFIED on silicon | **YES** -- direct grant + BSET/BCLR single-instruction discipline (PODR is RMW, no set/clear alias) |
| **ESP32-C6** (PMP) | **YES, best in fleet**: 8 B NAPOT over W1TS/W1TC (+0x08); IO_MUX + in-block FUNCn_OUT_SEL excluded | 4 B granularity, 16 entries (backend uses 8). Residue: bank-wide data bitmask, zero mux authority. Small-NAPOT-over-peripheral needs one silicon check | **YES** -- direct grant (atomic W1TS/W1TC) |
| **ESP32-WROOM** (LX6, no MPU) | MOOT -- no MPU, no privilege split | **NONE** (trust-only chip) | **YES trivially** (atomic W1TS/W1TC; nothing to isolate) |

ALLOCATION exclusivity is chip-INDEPENDENT (bookkeeping); REGISTER-grant exclusivity is a
chip-DEPENDENT floor -- exactly the isolation-ceiling pattern of 3.7 / section 2.1. The two chips
that cannot draw a per-pin line (XMC by PMSAv7 subregion math, K64F by having no peripheral gate
at all) are hardware ceilings of the class KickOS already documents, not an argument for a
kernel-mediated fleet default. Verdict: direct MMIO mode-2 CS is viable on every chip; the kernel
toggle is viable nowhere hot.

**Deferred to M4.4** (the driver batch that consumes this):
- **N MMIO windows per spawn** -- `thread_spawn` carries exactly ONE window today
  (`attr.mmio_base`); an XMC SPI driver with a mode-2 CS needs TWO (USIC channel + port), a C6
  driver its peripheral window + the W1TS/W1TC entry. Additive (a small bounded N, each
  admissibility-checked + region/PMP-budget-checked as today), no new object model. Zero windows
  needed on K64F / WROOM (open floor).
- **The vendor-neutral `kos_gpio` helper** -- `kos_gpio_claim_out` (cold arbitration + mux
  verify) returning a descriptor `{mode, set_addr, clr_addr, in_addr, mask}` with per-chip styles
  folded in, so `kos_gpio_set/clear/get` inline to one store/load (BSET/BCLR on RX) and the SAME
  driver logic runs on all five chips; `kos_gpio_require_direct` fails LOUD at bring-up
  (no-deferral) so a hot-CS driver never runs degraded on a chip that can only offer the kernel
  path.
- **Runtime mint-and-delegate** of a per-pin window to an already-running holder -- NO forcing
  consumer (every M4 CS is known at bring-up and served by grant-at-spawn); it is a kernel
  object-model change (generation / revoke-vs-running-holder / region-budget-at-mint) that must
  not be smuggled in through GPIO. Deferred until a dynamic-allocation consumer on a carvable chip
  exists.
- **Shared-IRQ demux** and any userspace GPIO service -- cold-path only, lands with its first
  real IRQ-consuming consumer; orthogonal to the toggle path (a shared GPIO IRQ line hardware-
  forces a demux, so IPC there is acceptable; a CS toggle never is).

Cross-ref the pinmux (3.1) + DMA (3.4) sections -- same shared-resource-vs-performance-vs-
isolation tension, different hot/cold profile. The allocation-vs-register ceiling is the
peripheral-isolation pattern (3.3 / section 2.1) applied at pin granularity.

---

## 4. THE MILESTONE-NUMBERING QUESTION (primary deliverable)

> **DECIDED 2026-07-20 -- Option A.** The user chose **driver era -> SMP -> MMU**: driver era =
> **M4**, SMP = **M5**, MMU / new-platform = **M6** (`roadmap.md`). The analysis below is retained
> as the RATIONALE; QW-3 (4.1) now carries **M5** (SMP's number). Work is still named by THEME
> where the number is not load-bearing, but the ordering is no longer open.

The tension: `roadmap.md` says **M4 = SMP** (one kernel image across cores); the user now
describes **M4 = the driver era**. The roadmap also tags the driver-era pieces (init service,
power-manager/clock-tree) as "anytime-coherence, whatever milestone number that carries,"
and puts the **MMU / new-platform horizon** (x86_64, i.MX8MP AMP) as post-M6, foundational.
This section lays out the ORDER of ALL remaining big rocks -- the user decides the numbers.

### 4.1 The three remaining big rocks + their enabling services
1. **DRIVER ERA** -- fleet UART/console drivers, per-chip reclaim, clock-select fleet-wide,
   the driver framework (call/reply IPC, taxonomy, multi-instance), and the enabling
   services: **init**, **clock-tree/power-manager**, **pinmux**, **gpio**. Single-core.
2. **SMP** (current roadmap M4) -- one kernel image across cores (RP2040/RP2350). Reworks the
   foundation: `IrqLock` ("IRQs off => exclusive") is single-core-only; plan = Big Kernel
   Lock first, then per-core run-queues. The AMP substrate is a de-risking stepping stone.
3. **MMU / new-platform** -- x86_64 PC + i.MX8MP heterogeneous AMP (MMU KickOS on A53 +
   MPU KickOS on M7 over cross-core IPC). Foundational, milestone-class, post-everything.

Parked item that MOVES WITH SMP's number: **QW-3** (`design-mmu-era-exploration.md:330`) --
keep the shared-IPC ring contract PHYSICALLY addressed from day one. It was flagged for
"M3/M4"; it belongs with the SMP/AMP cross-core IPC work, so it carries SMP's number -- now
**M5** (see the DECIDED banner above).

### 4.2 Dependency DAG across the big rocks
```
   DRIVER ERA (single-core)                    SMP (foundation rework)
   - fleet drivers + reclaim                   - IrqLock -> BKL
   - clock-select fleet                        - per-core run-queues
   - call/reply IPC (on CAP_ENDPOINT)          - AMP substrate (de-risk)
   - init / clock-tree / pinmux / gpio         - QW-3 phys-addressed ring
        |                                            |
        |  (delivers USABLE VALUE:                   |  (optimisation until there
        |   real apps can land)                      |   is a driver ecosystem to run)
        |                                            |
        +----------------------+---------------------+
                               v
                    i.MX8MP ENDGAME (needs BOTH)
                    - MMU KickOS on A53  +  MPU KickOS on M7
                    - heterogeneous AMP over shared IPC
                    - = driver-era drivers + AMP/IPC + MMU, together
                               ^
                               |
                    x86_64 PC (MMU, boot/APIC) -- MMU work,
                    largely independent of the driver era
```

Dependency argument:
- **Driver era does NOT depend on SMP.** It is entirely single-core work. It shares no
  invariant refactor with SMP -- the console/clock seams it uses are already single-core
  correct.
- **SMP is an OPTIMISATION until there is something to run.** One kernel across two cores
  with no driver ecosystem is a foundation with little payload. SMP's value is "run the
  driver/app workload at 2x," which presupposes the workload exists.
- **SMP shares NO refactor the driver era needs.** The `IrqLock`->BKL rework is invasive but
  orthogonal to the driver seams. The one overlap is IPC: the driver-era call/reply layer and
  the SMP cross-core ring are the SAME IPC lineage (control-plane sync + data-plane
  shared-mem async, per the IPC-performance spike). Doing call/reply FIRST (driver era)
  informs the cross-core ring; QW-3 (phys-addressed ring) should be honored in BOTH so the
  contract is portable.
- **The i.MX8MP endgame needs BOTH** driver era (drivers on the M7 + A53) AND AMP/IPC (SMP's
  cross-core lineage) AND MMU -- so it is strictly last regardless.
- **x86_64 is mostly independent MMU work** -- it needs the MMU + a new boot/interrupt model,
  not the driver era or SMP. It could slot anywhere the MMU work is scheduled.

### 4.3 Ordering options
- **Option A -- driver era first (RECOMMENDED, see below).**
  driver era -> SMP -> MMU/new-platform.
  PRO: delivers usable value now (real apps land, the "only KickCAT, one board" gap closes);
  SMP then has a workload to accelerate; call/reply-before-cross-core-ring is the natural IPC
  order; the init-service entry-rename breaking change is settled early. CON: the
  single-SMP-image end goal slips a milestone; the AMP de-risking is deferred.
- **Option B -- SMP first (keep roadmap M4=SMP).**
  SMP -> driver era (as anytime-coherence in parallel) -> MMU.
  PRO: honors the current roadmap; de-risks the multicore foundation while the codebase is
  still small; RP2350 M33 board momentum. CON: builds a foundation with little to run on it;
  the driver era (the thing that makes KickOS *usable*) waits; the call/reply IPC gets
  designed AFTER the cross-core ring (backwards); "anytime-coherence in parallel" is how the
  driver era has ALREADY been treated and it left the fleet at one console driver.
- **Option C -- split (interleave).**
  Driver-era CORE first (fleet drivers + reclaim + clock-select + init service = make M3
  real), THEN SMP (with the call/reply IPC + QW-3 done as its IPC front-half), THEN the
  driver FRAMEWORK maturation (call/reply drivers, DMA) + clock-tree service AFTER SMP.
  PRO: gets the fleet-wide "make M3 real" done fast (the user's stated goal), settles the
  init-entry breaking change early, then does SMP while the heavier framework/DMA design
  bakes. CON: splits the driver era across two numbers -- the framework work is separated
  from the drivers that motivate it.

### 4.4 Recommendation (for the user to accept or override)
**Option A: the driver era is the next milestone (make M3 real across the fleet), SMP
follows, MMU/new-platform last.** Rationale: (1) the user's own framing -- "M3 = POC, the
next thing = make it real across the fleet" -- IS the driver era; (2) it delivers usable
value (real apps can land) whereas SMP is an optimisation with no workload yet; (3) the IPC
order is right (call/reply informs the cross-core ring, not vice versa); (4) it forces the
init-service entry-point rename NOW, while consumers are few. If the RP2350-M33 / RP2040 SMP
hardware momentum is the stronger pull, Option C is the honest compromise: land the
"make-M3-real" core first (drivers + reclaim + clock-select + init), then SMP, then the
heavier framework (call/reply drivers, clock-tree service, DMA).

Now DECIDED (Option A, see the banner atop this section): driver era = M4, SMP = M5, MMU = M6.
The doc still names the work by THEME ("the driver era") where the number is not load-bearing,
because the roadmap's own "anytime-coherence" tagging means several of these pieces are not
strictly gated by number.

---

## 5. PRIORITY / SEQUENCING (what unblocks what)

Within the driver era, dependency order:
1. **Init-service entry-point seam** (G7) -- rename EARLY; a cheap-now/break-later quick win.
   Foundational: everything spawns through init. Mostly **NOW** (sim + build).
2. **Foundational services** (section 3.1): **clock-tree**, **pinmux**, **gpio** -- the
   drivers stand on these. Design gate first (central-vs-grant for pinmux is open). Clock-tree
   builds on the G4 mechanism seam. Mix of **NOW** (design/sim) + **HW** (validate).
3. **Call/reply IPC layer** on CAP_ENDPOINT (section 3.2) -- the substrate for SPI/I2C
   drivers; the reply-cap is the missing half. **NOW** (sim/QEMU, like the endpoint proof).
4. **Fleet console drivers + reclaim** (G1/G2/G3), silicon-available first: XMC done ->
   RX72M, ESP32-C6 (each gated on its G5 prereq) -> K64F (reclaim proof) -> STM32 family +
   RP2040. Per-chip **HW**.
5. **Clock-select fleet** (G4) -- per opt-in chip; **HW**. Then the **power-manager /
   clock-tree service** (G7) as the policy layer on top -- **HW + design**.
6. **Driver framework maturation** (G6) -- the lib/demo split + typed contract. **NOW**
   (design + sim), validated **HW**.
7. **DMA** (3.4) -- DEFERRED sub-topic; polled/IRQ drivers first.

Silicon-gated (need boards on a bench): G1 per-chip drivers, G2 reclaim validation, G3
handover validation, G4 clock-select, G5 C6-APM + RX-IRQ-demux, and all HW re-validation.
Doable NOW (in-tree / QEMU / sim / design): the init-entry seam, the call/reply IPC layer
(like the endpoint proof, K64F+XMC+QEMU), the driver-framework design + lib/demo split, the
pinmux central-vs-grant decision, the m2-review-followups read, and the milestone-numbering
decision itself.

### 5.1 Prereq / blocker summary
- ESP32-C6 console driver  BLOCKED BY  C6 APM/PMS global open (G5, `design-c6-driver.md`).
- RX72M IRQ-driven driver / ring  BLOCKED BY  the RX peripheral-IRQ demux stub (G5).
- SPI/I2C userspace drivers  BLOCKED BY  the call/reply IPC layer (3.2).
- Every fleet console driver  ENABLED BY  the xmcuart template (exists) + per-chip reclaim (G2).
- Ordered driver bring-up  ENABLED BY  the init service + foundational services (3.1, G7).
- The clock-tree rate-change fan-out  MATTERS ONLY ONCE  multiple derived-clock consumers
  (drivers) exist -- so clock-tree SERVICE follows the first drivers, though the clock
  MECHANISM (G4) can precede them.

## 6. M4 design decisions (fable review 2026-07-20 + review discussion)

Adversarial review in `design-m4-fable-review.md`; these are the accept/revise calls that
supersede the earlier prose above where they differ.

- **Clock: there IS an M4 service, scoped to the SAFE parts; only the live cascade defers.**
  Three-way split: (1) a clock ORACLE -- a driver queries its PARENT/BRANCH clock (a UART
  needs its fPERIPH for baud, SPI its prescaler); `sys_cpu_clock_hz()` only gives the CORE
  clock, so this is a real, read-only, cascade-free need, AND it is the seam a later
  rate-change notify walks. (2) one-shot BOOT tree config/select+gate per branch (pinmux-
  shaped, privileged pokes behind the kernel seam). (3) the live DVFS rate-change CASCADE
  (cross-domain notify -> quiesce -> re-derive, a two-phase commit across untrusted drivers)
  is DEFERRED and built against the CONSOLE as the first forced notify instance (M3 today
  merely REFUSES a retune while the console is USER_OWNED -- `clock_select.cc:32`, verified
  correct; that refuse-path is where the handshake grows). Authority is a SYSCALL-GATING
  capability, never an SCU/RCC MMIO grant (that window ungates any peripheral / kills the
  kernel timer clock); the kernel keeps every shared-clock-block register, so a service bug
  is restartable policy, not a flash/PLL hard fault. Fixes the roadmap "Later/clock-tree" prose.
- **GPIO: direct-MMIO grant, kernel does only pinmux (finding 9 superseded by the spike
  `design-m4-gpio-direct-spike.md`).** The KickCAT ESC SPI needs a driver-owned CS (multi-device
  buses, boards with no HW-PCS pin, CS held across FIFO refills), so mode-2 direct GPIO CS is IN
  M4 -- served by the grant-at-spawn MMIO mechanism (task #9); a hot pin NEVER routes through a
  syscall (~100-200-cycle SVC vs a 222 ns CS edge, section 3.5). A kernel pin allocator + toggle
  syscall was built (af7d99a) then REMOVED once the latency spike confirmed it is non-viable for
  a hot pin. The earlier "expose an atomic set/clear window excluding the mux" homework is CLOSED
  as a documented NEGATIVE result on XMC: OMR (+0x04) and IOCR0-12 (+0x10-0x1C) share the one
  32 B PMSAv7 min-region/subregion, so any region granting the toggle grants that port's remux --
  address arithmetic, not a silicon question. The XMC answer is a dedicated-port grant (board
  layout makes the over-grant harmless) or a trusted over-grant, the K64F-coarse-AIPS ceiling
  class. Pinmux stays a one-shot privileged init step, verified (not set) by the cold claim path.
  Shared-port IRQ demux (if ever needed) is a non-blocking per-subscription notification with
  sticky-pending, ack ISFR before delivery -- NEVER a parked rendezvous send that would let one
  slow subscriber deafen the port. DEFERRED to M4.4: N-windows-per-spawn, the vendor-neutral
  `kos_gpio` helper, runtime mint-and-delegate (no forcing consumer). See section 3.5.
- **Call/reply must carry the scheduling contract (finding 4).** Synchronous SPI/I2C over
  CAP_ENDPOINT without priority donation = unbounded inversion on every transaction (KickCAT
  cyclic traffic is the victim). The reply-cap design gate MUST include direct-handoff /
  priority donation on call (as sem_post's token handoff already does), else it is not an
  RTOS API.
- **Transfer ABI is offset-based from day one (finding 10, the one M6 landmine).** A large-
  transfer request speaks {region-cap, offset, len}, never a raw pointer -- cheap now, an
  ABI break at M6 when a domain becomes a page-table root. Pull the QW-3 DISCIPLINE (not the
  ring impl) into the M4 call/reply gate.
- **Timed / abortable IPC -> EARLY-M4.** Gates BOTH the clock-cascade quiesce-timeout and a
  driver-death waiter wake. Open since the handover spike; call/reply makes it load-bearing.
- **Driver crash/restart + resource reclaim** (pin caps, clock-gate refcounts, AIPS slots,
  endpoint holders) is a named M4 gap -- only the panic-path console reclaim exists today.
- **RP2350 M33 arch question (settle in this pass).** The M33 is ARMv8-M Mainline; KickOS rides
  the `armv7m` layer for the core (NVIC/SysTick/SVC/BASEPRI/regfile are a v7-M superset --
  first silicon 40/40 mpu-off) and diverges only at the MPU: PMSAv8 (RBAR+RLAR+MAIR) is a
  different unit from v7-M PMSAv7 (RBAR+RASR), so enforcement needs a new v8-M backend
  (`design-rp2350-mpu-armv8m.md`). Decide: "armv7m board + v8-M MPU backend" vs a real
  `armv8m` arch split. Gates M4 (RP2350 enforcement) and M5 (dual-M33 SMP endgame).

---

## 7. API-CONFORMANCE OBJECTIVE (the neutrality matrix)

The gap list above is the WORK; this section pins WHY M4 does it. The objective is not "ship
drivers" for its own sake -- full driver support is the VEHICLE that validates the M4 service
APIs against real hardware variation. The end deliverable is a set of console/UART, gpio,
pinmux, clock/power, and bus (SPI/I2C) service APIs proven GENUINELY VENDOR-NEUTRAL -- not
accidentally shaped around one vendor's register model, clock tree, or pin scheme. A driver
that lands cleanly on one vendor and then forces an API change to land on the next is the
signal that the API leaked a vendor assumption; the matrix is how that leak gets caught.

### 7.1 The four-board neutrality matrix
Four easy-to-flash boards, chosen for DIVERSITY across BOTH axes at once -- vendor AND
arch/MPU family:

| Board | Arch | MPU family | Vendor |
|---|---|---|---|
| c6 (ESP32-C6) | rv32imac | PMP | Espressif |
| xmc (XMC4800) | armv7m | PMSAv7 | Infineon |
| k64f (FRDM-K64F) | armv7m | SYSMPU | NXP |
| rx72m | RXv3 | RX-MPU | Renesas |

Four vendors x four arch/MPU families is a far stronger API-neutrality test than adding
another ARM board: two boards can share an ISA (xmc, k64f are both armv7m) yet still exercise
DIFFERENT MPU units (PMSAv7 vs SYSMPU) and DIFFERENT vendors, and the two non-ARM boards (c6
rv32imac/PMP, rx72m RXv3/RX-MPU) stress the API where an ARM-only fleet is silent. A vendor
bias baked into a service API cannot survive being ported across all four.

### 7.2 The driver->board mapping principle (the scope guard)
"Total / full driver support" is an ASPIRATION to FULL per-board peripheral coverage, NOT a set
bounded a-priori to the peripheral CLASSES the M4 services define (console/UART, gpio, pinmux,
clock/power, plus at least one bus SPI and/or I2C). Reaching for a board's WHOLE complement is
how we discover hardware that needs a dedicated / new API -- that discovery IS the point. The
bound is a COMPLEXITY-vs-GAIN weighting per candidate, NOT a class restriction: the per-board
prioritized backlog + the cross-board neutrality shortlist in `docs/design-m4-driver-matrix.md`
are what keep scope from ballooning into a four-vendor BSP while still exercising each API --
and surfacing the high-gain NON-class peripherals (analog-in, PWM, the C6-ETM/XMC-ERU/RX-ELC
event fabric, the xmc/rx72m on-chip EtherCAT SC) that a class restriction would hide.

So the discipline is: prefer MAPPING different drivers to different boards to maximize
variation coverage, over duplicating one driver across all four. A given service API is
"neutrality-proven" once its class has a working driver on boards spanning the vendor/arch
spread -- not once every board carries every driver. Console/UART is the exception that DOES
want all four (see 7.3); the rest earn their proof by covering the diversity axes, not by
uniform replication. This keeps M4's scope from ballooning into a full BSP for four vendors
while still exercising each API against enough hardware to expose a vendor assumption.

### 7.3 The no-probe constraint (a design INPUT, not an afterthought)
Two of the four matrix boards -- rx72m and c6 -- are easy to FLASH but have NO usable debug
probe. Bring-up on them is PRINT-DEBUG ONLY: no single-step, no register peek over a probe,
only what the board can print. This is a hard input to the service design, not a testing
footnote: it means the services must be CONSOLE-OBSERVABLE to be brought up on those two
boards at all. If a driver / service can only be diagnosed through a debugger, it cannot be
brought up on half the neutrality matrix.

Consequences:
- Console/UART is the FIRST driver on every matrix board (not just the interesting-isolation
  ones), because it is the observability substrate every other bring-up on rx72m/c6 depends
  on. This is why console/UART is the one class that wants all four boards (7.2).
- The M4 services should surface bring-up state (init progress, allocation/grant results,
  clock/baud derivation, transfer status) over the console, so a print-only board can
  diagnose a failed handover / mux / clock-select without a probe.
- The porting invariant already in play (a real `arch_console_reclaim`, section 1 / G2)
  matters MORE here: on a no-probe board a garbled UART that eats the panic banner leaves
  NO diagnostic channel at all.
