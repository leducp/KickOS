<!-- SPDX-License-Identifier: CECILL-C -->
# M4 driver-era -- adversarial design review

An adversarial review of the M4 "driver era" design PRINCIPLES, run 2026-07-20 before
M4 implementation starts. Sources reviewed: `docs/design-driver-era-scope.md`,
`roadmap.md` (M4 + the "Later" init / power-manager / clock-tree prose + M5/M6),
`docs/design-spi-driver.md` + the K64F/F411 instances, `docs/design-m3-console-handover.md`,
`docs/reference/invariants.md`, plus spot-checks of landed code (`kernel/time/clock_select.cc`,
`kernel/init/console.cc`, `arch/arm/chip/xmc4800/usic_uart.cc`, `kernel/include/kickos/config/system.h`).

The job was to find holes, not to validate -- so every finding is stated at its strongest.
Findings are the reviewer's; the VERIFICATION notes below are the outcome of checking the
strongest claims against the code afterwards.

## Verification addendum (checked after the review)

- **Finding 1 (the top BLOCKER) does NOT hold against the landed code.** `clock_select.cc:32`
  early-returns `if (console_owner_is_kernel() == 0) return arch_cpu_clock_hz();` BEFORE any
  masking or console touch -- the S4 refusal is landed and guards the whole retune, including
  the `console_tx_flush_sync()` / `arch_console_retune()` at lines 49-62 the reviewer flagged.
  The reviewer saw the unconditional-looking calls but missed the line-32 gate above them. So
  the M3 clock-select code is correct as shipped.
  - BUT the underlying M4 concern survives: "refuse while USER_OWNED" is the right M3 policy for
    ONE console, and it does NOT scale to a driver era where a userspace driver owns the console
    (or any derived-clock peripheral) yet you still want DVFS. The refuse-path is exactly where the
    rate-change notify/quiesce handshake (finding 6) must replace it. See the clock discussion below.

The other findings are about M4 DESIGN (not landed code), so they are not falsifiable the same
way; they stand as design risks to weigh.

---

## Findings (ranked most-severe first)

### 1. Clock-select touches a console the kernel may not own  -- FALSE ALARM (see addendum)
**Principle:** clock-tree / G4 fleet clock-select. Reviewer severity: BLOCKER.
Reviewer claim: `clock_select.cc:50,62` call `arch_console_flush_sync()` + `arch_console_retune()`
with no `g_console_state` check, so a privileged retune while the console is USER_OWNED has the
kernel writing baud into a user-owned device, violating the handover invariant (`console.cc:45`).
**Verification: refuted** -- the line-32 `console_owner_is_kernel()` early-return guards it. The
M4-relevant residue (refuse does not scale to DVFS-with-userspace-drivers) is real and folds into
finding 6.

### 2. The per-pin GPIO cap fails on both silicon-proven chips
**Principle:** GPIO pin allocator; collides with pinmux-privileged. **Severity: BLOCKER (for the
"unified model" claim as stated).**
On XMC PMSA the minimum region is 32 B from the port base and `IOCR` sits at port+0x10 --
`design-spi-driver.md` already establishes a data-register window cannot be split from the mux at
this granularity, so a minted "per-pin" cap hands the holder pin-REMUX escalation (it can re-mux
other drivers' pins -- MORE than spawn-time grants ever allowed). On K64F the minted cap is inert:
AIPS opens the slot to all user code. So on the two chips where the grant model is silicon-proven,
the flagship principle either escalates or enforces nothing. Scope 3.5 pre-empts with mitigations
(a)-(d) but understates the XMC case as "poke a co-resident pin" -- it is system-wide remux escalation.
**Recommendation:** mint only where the chip's atomic set/clear + input registers are encodable in a
mux-free window (per-chip register-map homework as a gate, like the G2 reclaim lists); otherwise fall
back to a syscall-validated toggle or trusted-driver over-grant, explicitly, per chip. Do not present
the cap as the model and the ceiling as a footnote.

### 3. "Runtime minting of an MMIO cap" is a kernel object-model change, not an extension
**Principle:** GPIO (scope 3.5 "keystone"). **Severity: MAJOR (needs-a-spike-before-M4).**
Today MMIO is an MPU region fixed at spawn (task #9 grant-at-spawn); `invariants.md`
`object-access-via-per-task-cap` is explicit that memory R/W/X stays in the MPU descriptor, not the
cap. There is no MMIO cap object, no generation, no teardown. Minting at runtime mutates a LIVE
thread's region set (invisible until the next switch-in; a revoke leaves the window writable until
then). Region budget: PMSA has 8, SPI already spends 4, each pin cap is +1 -- a driver with an SPI
window + two GPIO pins on different ports is at 6-7/8 before any domain grant. Revocation-on-death
presupposes the crash/reclaim machinery that does not exist (finding 5).
**Recommendation:** spike the MMIO-cap object (generation, revoke vs a running holder, region-budget
accounting at mint, interaction with `arch_mpu_region_encodable` fail-closed) before M4 commits the
GPIO shape. Finding 9 argues the first consumer may not exist yet.

### 4. Call/reply with no priority donation = unbounded priority inversion per transaction
**Principle:** synchronous call/reply on CAP_ENDPOINT. **Severity: MAJOR.**
A high-priority client calls `spi_transfer`; it is served at the DRIVER's static priority; any
medium-priority thread preempts the driver indefinitely -> the client's deadline blows on an
unrelated thread. The handover spike explicitly rejects PI on endpoints (correct for the async
console, wrong for a synchronous transaction path). The doc name-drops "the L4 call/reply fastpath"
but L4's call is correct BECAUSE of direct-handoff / timeslice donation -- importing the reply-cap
without the scheduling half imports the API and drops the guarantee. Victim: KickCAT's EtherCAT
cyclic exchange over the K64F DSPI driver.
**Recommendation:** the reply-cap design gate must include the scheduling contract (direct handoff on
call -- client donates priority or the switch goes straight to the driver, as `sem_post`'s token
handoff already does -- and back on reply). Without it, "synchronous driver API" is not an RTOS API.

### 5. No death story on the call path: a crashed driver parks its clients forever
**Principle:** call/reply + the missing-entirely list. **Severity: MAJOR.**
Client blocks on the reply; the driver faults (the event the fault-funnel celebrates catching). The
client is parked with no timed send/recv (explicitly OPEN), no endpoint-death wakeup for parked
waiters, no driver restart. Mirror case: client dies mid-call -> the driver's reply targets a dead
cap (must be a cheap no-op; unspecified). The dead driver also leaks its pin caps, clock-gate
refcounts, AIPS slot, endpoint slots. The only reclaim that exists is the panic-path console reclaim
-- a whole-system-death path, not a driver-death path.
**Recommendation:** decide/spike the minimum: timed/abortable IPC + "endpoint destroyed wakes all
parked waiters with -1" + a stated position on whether driver death is system-fatal in M4 (honest if
chosen) -- but then say so, because scope 3.5 promises "on driver-death the cap is revoked and the pin
freed", machinery that does not exist.

### 6. The clock-tree service contradicts its own bring-up DAG; the notifier cascade is unsafe
**Principle:** clock-tree / power-manager. **Severity: MAJOR.**
Internal contradiction: scope 3.1's DAG makes CLOCK-TREE a foundational service init brings up
BEFORE gpio and drivers; scope G7's verdict says it comes AFTER the clock MECHANISM (G4) and the
first drivers. Both cannot be true. (Cheaper resolution: the DAG's real dependency is only "gate the
driver's clocks at bring-up" -- a one-shot init step like pinmux, no standing service.) The cascade:
"Linux CCF shape" hand-waves over the fact that CCF notifiers are same-address-space calls under a
mutex, while here each notify is cross-domain IPC. A rate change during an in-flight SPI EOQ or a UART
frame corrupts the wire, so the fan-out needs a PRE-quiesce (drain/park) and POST phase -- a two-phase
commit across N untrusted driver threads, where one slow/dead driver stalls DVFS forever (no timeout,
finding 5), and a driver whose notify handler calls the clock service re-enters a single-threaded
service parked mid-cascade.
**Recommendation:** DROP the standing clock-tree service from the M4 principle set. Keep (i) init
one-shot gating (satisfies the DAG), (ii) the G4 kernel mechanism with the finding-1 console handshake
as the FIRST forced instance of the notify protocol. Design the full service against that proven
instance, late-M4 at the earliest.

### 7. The "delegatable clock-control capability" is undefined, and its MMIO reading is an escalation surface
**Principle:** clock-tree / power-manager. **Severity: MAJOR.**
Roadmap Later/clock-tree: the service "holds it like a driver holds an MMIO grant." If it IS an MMIO
grant it is a window over SCU/RCC/SIM -- the block both SPI briefs name as the kept-privileged
escalation surface ("SCU could ungate any peripheral"; "SIM clock gates -> escalation"), and on XMC
the SCU also carries reset/watchdog/trap control at grant granularity. Then the roadmap safety claim
-- "a service BUG is wrong policy (restartable), not a flash-controller hard-fault" -- is FALSE: a
scribble can kill the kernel timer's clock source or the console's fPERIPH. Same paragraph also
contradicts itself: the service "OWNS the PLL, dividers/muxes" AND "PLL relock stays kernel-side
residue."
**Recommendation:** state explicitly that the clock-control cap is SYSCALL-gating, never an SCU/RCC
MMIO window; the kernel owns every register in the shared clock block; the service holds policy
authority only (which subtree / which P-states -- a rights-bearing kernel-object cap, a design gate).
Fold into the finding-6 spike.

### 8. Shared-IRQ GPIO demux over rendezvous IPC is a cross-driver DoS and a priority launderer
**Principle:** GPIO (the IRQ-demux half). **Severity: MAJOR.**
The GPIO service owns the shared PORTx line, reads ISFR, delivers the per-pin event via IPC, acks
after delivering. With the landed rendezvous endpoint, "deliver" = `kos_send`, which PARKS the service
if the subscriber is not already in recv. One slow/dead subscriber stalls the demux thread; the line
stays masked (re-arm only on the service's next `irq_wait`), so EVERY pin on that port goes deaf --
driver A's bug silences driver B's interrupts, precisely the isolation the allocator claims. Also: all
subscribers' edge handling runs at the service's single priority (inversion either way). Path is HW IRQ
-> tier-1 wake -> service thread -> IPC -> owner wake: two switches + one IPC per edge.
**Recommendation:** delivery must be non-blocking per subscriber (a per-subscription
semaphore/notification with a sticky-pending / overflow-counter policy, never a parked send), and the
service acks ISFR immediately after latching, before delivery. State the latency class honestly.

### 9. YAGNI: the motivating consumer for minted pin caps (SPI chip-select) was already rejected
**Principle:** GPIO; scope internal consistency. **Severity: MAJOR (over-reach signal), cheap to fix.**
Scope 3.1 annotates SPI with "{clock, a GPIO cap for its CS}" and 3.5 opens with the hot-CS-toggle
motivation. But `design-spi-driver.md` ("GPIO CS rejected on XMC") and the K64F brief ("HW PCS is
strictly cleaner") both fold CS into the peripheral window and reject the GPIO route; the F411 brief
uses software NSS -- no GPIO cap. So on all three designed SPI instances the MHz-hot-path argument for
the runtime-mint keystone has NO consumer. What remains for M4 GPIO is cold stuff (a KickCAT run-LED, a
button) that a validated kernel set/clear syscall covers with zero new kernel object model.
**Recommendation:** defer the mint mechanism (finding 3's spike loses urgency); ship the GPIO service in
M4 as allocator-bookkeeping + syscall-mediated toggle + the (fixed, per finding 8) demux; revisit minted
caps when a real MHz-rate GPIO consumer appears (a bit-banged bus). Fix the 3.1 DAG annotation.

### 10. The large-transfer shared-buffer ABI is M4 work carrying an M5 label -- the one M6 landmine
**Principle:** call/reply + ordering. **Severity: MAJOR (one-paragraph fix now, driver-ABI break later).**
`KOS_EP_MSG_MAX` bounds the inline payload; scope 3.2 says larger SPI transfers "want a granted shared
buffer" and notes this is "the same physical-addressing discipline QW-3 flags" -- but scope 4.1 parks
QW-3 with M5. If M4's driver ABI passes raw pointers into a shared region (natural today where
virtual==physical), every driver contract breaks at M6 when a domain becomes a page-table root: the
client's pointer means nothing in the driver's address space.
**Recommendation:** pull the QW-3 DISCIPLINE (not the ring implementation) into the M4 call/reply gate:
the wire format of a transfer request is {region-cap, offset, len}, never a raw address, from day one.

### 11. The default init's cap set is a silent ABI, and init's lifecycle is unspecified
**Principle:** init service. **Severity: MINOR.**
The B1 placement contract makes cap handles positional (index 0 reserved, delegated cap i at child
index i+1). "A default init that wires a sane cap set" means every addition SHIFTS every subsequent
index -- an app that learned "SPI endpoint is handle 2" breaks silently when a clock cap is inserted at
2. The doc flags the entry RENAME as the consumer break (correct) but misses that the cap-set LAYOUT is
the recurring one. Also unspecified: does init exit after bring-up (then nobody serves dynamic pin
re-config or driver restart) or persist (a permanently-live privileged root, the largest trusted body
in the system)? LOW-BARRIER itself survives: a per-BOARD in-tree pin map is not a per-APP manifest.
**Recommendation:** freeze a well-known-index convention (or a get-cap-by-name query) in the same gate as
the rename; decide init's persist-vs-exit posture explicitly.

### 12. RAM/slot budget math is absent for the service + thread-per-instance shape on small parts
**Principle:** call/reply + GPIO. **Severity: MINOR.**
`KICKOS_MAX_ENDPOINTS` defaults to 4; badging burns a pool slot per badged client, plus (future) one
per in-flight reply. Init + clock + GPIO + one UART + one SPI driver is already 5+ endpoints and 4-6
thread stacks before the app runs -- on F302 the scope's own table says "RAM-tight", and on no-MPU parts
(F103, LX6) the thread+IPC shape buys zero isolation over a library call while paying full freight.
**Recommendation:** add a worst-case RAM/slot budget per board class to the framework gate, and explicitly
permit the degenerate "driver as a linked library, no service thread" configuration on no-MPU parts.

---

## What's missing entirely
- **Driver crash/restart + resource reclaim** (pins, clock refcounts, AIPS slots, endpoint holders) --
  only the panic-path console reclaim exists.
- **Timed / abortable IPC** -- explicitly OPEN since the handover spike; call/reply makes it load-bearing.
- **Driver ABI versioning + cap discovery convention** -- M4 mints the first multi-consumer contracts;
  even a frozen well-known-index table is a decision.
- **Multi-device bus model** -- one SPI bus with several CS lines / one I2C bus with several addresses:
  per-device config (CTAR/speed/mode), transaction bracketing (I2C repeated-start atomic against other
  clients), CS ownership. Thread-per-INSTANCE answers none of this.
- **Error taxonomy across the reply** -- {status, rx bytes} is named but NAK / arbitration-lost / timeout
  / revoked-mid-transfer are not.
- **Power/idle interplay** -- DVFS + tickless WFI + gated clocks (which sources survive the chosen sleep
  state; who may gate the timer's branch) is nowhere, yet it is the power-manager's first real problem.
- DMA + its shared-channel allocator: genuinely missing but the doc pre-empts it well (scope 3.4's
  defer-with-a-named-shape is the right call).

## Ordering verdict (M4 -> M5 -> M6)
The ordering HOLDS, and scope 4.2-4.4 is the strongest part of the doc: the driver era has no
dependency on SMP; SMP without a workload is a foundation with no payload; call/reply-before-cross-core
-ring is the right IPC lineage. The M4 principles are mostly SMP-safe by construction (thread-per-instance
serializes per peripheral; "atomic set/clear or don't mint" is the SMP-proofing needed anyway -- caveat:
RX72M has no atomic GPIO set/clear, but it is not an M5 SMP target). Two things must be pulled ACROSS the
boundary rather than reordering it: the QW-3 offset-based buffer discipline belongs in the M4 driver ABI
(finding 10), and the init entry rename + cap-layout convention belong at the very front of M4 (finding 11)
-- both cheap-now / break-later seams. Nothing in M5 or M6 argues for going first.
