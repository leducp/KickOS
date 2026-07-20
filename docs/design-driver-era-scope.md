<!-- SPDX-License-Identifier: CECILL-C -->
# The driver era -- scope / gap analysis

**EXPLORATORY -- NOT A CONTRACT.** A scoping lens over the work that turns the M3
mechanisms (proven on XMC / K64F only) into a real, fleet-wide capability. Numbering is
deliberately IN FLUX (see "The milestone-numbering question"), so this doc names work by
THEME -- "the driver era" -- not by Mx. No implementation here; a checklist lands in
`TODO.md` and the design in `docs/reference/` only after the user picks a number + gate.

Framing (user's words): **M3 = POC** -- endpoints/IPC, console handover, clock-select, the
fault-funnel reclaim all PROVEN, but each on ONE or TWO chips. **The driver era = the real
deal** -- reuse the M3 mechanisms and make them real ACROSS THE FLEET, fixing whatever gaps
XMC-only (+ some K64F) testing hid. The console handover was never tried fleet-wide.

---

## 0. Outline

1. State of the M3 mechanisms (what landed, on which silicon)
2. Gap list -- the driver-era work, per item, effort + dependency + silicon-gating
3. Driver-framework depth -- the bring-up DAG, the API taxonomy, multi-instance, DMA
4. The milestone-numbering question -- driver-era vs SMP vs MMU, ordering options + rec
5. Priority / sequencing -- what unblocks what; silicon-gated vs doable-now

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
subject of section 3 (the API taxonomy). Effort **L** (design gate first).

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
| STM32F103 (bluepill) | USART1 @0x40013800 (old SR/DR), PA9 | none | polled | GAP | 4 (shares F411 old-model driver) |
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
                 +----------------+
                 |  CLOCK-TREE    |  (PLL, dividers/muxes, central refcounted
                 |  service       |   tree-gates; rate-change-notifier fan-out)
                 +----------------+
                    |         |
          +---------+         +---------+
          v                             v
   +----------------+            +----------------+
   |  PINMUX        |            |  (kernel clock |
   |  service       |            |   residue:     |
   | (per-pin auth, |            |   re-anchor)   |
   |  central like  |            +----------------+
   |  the clock tree|
   |  OR per-grant) |
   +----------------+
       |     |     |
       v     v     v
   +------+ ...    (every peripheral needs a pin route)
   | GPIO |  <--- foundational leaf: raw pin drive; also the SPI-CS provider
   +------+
       |
   ----+-------------------------------+------------------+
   v                                   v                  v
+------+ needs {clock,pinmux}      +------+ needs      +------+ needs
| UART |                          | I2C  | {clock,    | SPI  | {clock,pinmux,
+------+                          +------+  pinmux}    +------+  GPIO for CS}
                                                          ^
                                              GPIO edge: SPI CS is usually a
                                              GPIO line, so SPI depends on GPIO
```

Foundational services (the three the peripheral drivers stand on):
- **CLOCK-TREE** (G7) -- owner of the shared PLL/dividers/central gates. A rate change
  cascades to every derived-clock consumer (UART re-derives baud, SPI its prescaler) via a
  Common-Clock-Framework-shape notifier. Central + refcounted (a branch feeding two
  peripherals gates off only when BOTH idle). Kernel residue = re-anchor its own clock.
- **PINMUX** -- a shared per-pin authority. Same design question as the clock tree: CENTRAL
  authority (a pinmux service owns all pin-mux registers, hands routes) vs PER-DRIVER GRANT
  (each driver granted its pin's IOCR/PORT window). Pin-mux registers often live in a SHARED
  block (XMC P1_IOCR4, K64F PORTx_PCR, an SCU/PORT peripheral) -- exactly the clock-tree
  "shared block => central refcount" argument -- so PINMUX likely wants to be central too,
  NOT a per-thread grant (the grant model can't cleanly split one PORT register across
  drivers). This is a real open design decision.
- **GPIO** -- foundational leaf: raw pin drive; ALSO the SPI-CS provider, so SPI depends on
  it. GPIO itself needs pinmux (route the pin to GPIO function) + clock (port clock gate).

Consequence: the init service is a topological bring-up (clock -> pinmux -> gpio -> the
byte/transfer drivers -> apps), spawning each with the right caps. This is why the init
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

---

## 4. THE MILESTONE-NUMBERING QUESTION (primary deliverable)

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
"M3/M4"; it belongs with the SMP/AMP cross-core IPC work, so it carries SMP's number wherever
that lands.

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

Whatever the choice: keep the doc + `TODO.md` **milestone-number-neutral** and name the work
by THEME ("the driver era") until the user assigns numbers, because the roadmap's own
"anytime-coherence" tagging means several of these pieces are not strictly gated by number.

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
