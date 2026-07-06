<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS telemetry / kernel-observability — reference

> This is the DURABLE reference for the telemetry subsystem (record/wire format,
> frontend/backend, decoder contract, CI gates) — it stays in sync with the code
> (`kernel/ktrace/`, `include/kickos/trace/`, `tools/kicktrace.py`), unlike a
> throwaway spike. Landed as M1.x step 11d. Section numbers ("deliverable N",
> "CI gate N", "G6") are the stable index the code comments cite.

**Status: DESIGN, revised after a 3-pass adversarial review — nothing implemented.**
This finalizes the Phase-2 telemetry items that
[`console-design.md`](console-design.md) left open, and folds in the review's
findings (two independently-confirmed critical ones) plus the design decisions
taken in review. Read `console-design.md` first; this does not restate the locked
architecture (frontend/backend split, stream 1+ on the console core, RTT sink,
non-perturbation rule, completion-not-decision stamping).

Charter note: telemetry is **kernel observability — benchmarking *and* debugging
from one instrumented stream**, not benchmarking alone (see §12).

## Why now (and why before M2)

M2 reprograms the MPU on every context switch-in (`sched::switch_to` already calls
the no-op `arch_mpu_apply`). Measuring the MPU's per-switch cost *after* it lands is
measuring blind. So 11d captures switch / syscall / IRQ latency and CPU% **now**,
while the switch path is still bare (privilege+SVC only), and **defends a baseline
before M2 stacks the MPU on top**. That baseline is defended on **XMC4800
(armv7m / DWT, cycle-accurate)** — see §1 for why not RP2040.

## Prior art (NuttX, FreeRTOS — grounded in the local trees, credited not ranked)

Both converge on the exact shape here, which validates it:
- **FreeRTOS**: empty **trace-hook macros** gated by `configUSE_TRACE_FACILITY`
  (`traceTASK_SWITCHED_IN/OUT` at `tasks.c:3767`, `traceISR_ENTER/EXIT`), `#define`d
  to a recorder → our compile-out `ktrace_*`. CPU% via `configGENERATE_RUN_TIME_STATS`
  + a user hi-res counter `portGET_RUN_TIME_COUNTER_VALUE` → our `arch_trace_now`.
  Recorders: Percepio Tracealyzer (snapshot RAM-ring or streaming) + SEGGER SystemView.
- **NuttX**: `CONFIG_SCHED_INSTRUMENTATION` with **per-event-class sub-flags**
  (`_SWITCH`, `_SYSCALL`, `_IRQHANDLER`, `_PREEMPTION`, `_HEAP`, `_FILTER`); kernel
  calls `sched_note_*` ("notes") into a **note-driver** abstraction with many
  backends: `noteram_driver` (RAM ring at `/dev/note` = flight recorder),
  `notesnap`, `notestream`, `noterpmsg` (cross-core), `drivers/segger/note_{rtt,
  sysview}.c`. CPU% via `CONFIG_SCHED_CPULOAD`.

Borrowed here: (a) NuttX's **per-event-class enable** as our config growth path
(§6); (b) `noteram` = the **flight-recorder-on-halt** (§12); (c) a **Perfetto /
Chrome-trace** decoder output (NuttX emits CTF/Perfetto) — free, powerful viewers
(§7). Deliberate divergence: FreeRTOS accumulates CPU% **on the target**
(`portGET_RUN_TIME`); we derive it **host-side** from the SWITCH stream (leaner,
non-perturbing) — §3.

---

## 1. `arch_trace_now()` — the cycle-probe seam (armv7m-uniform / armv6m-chip-capability)

New in `arch/include/kickos/arch/arch.h`, next to `arch_clock_now`:

```c
// Free-running high-resolution trace counter, RAW CYCLES/TICKS (not ns). A plain
// register read -- no lock, no I/O; it is on the measured path. Its width and rate
// are published in the session header (§2); the host converts. Distinct from
// arch_clock_now() (ns, 64-bit, coarser). Its width is arch/chip-specific: see
// SESSION.ts_bits.
uint32_t arch_trace_now(void);
```

**Decision: raw counter, host converts** (in-target ns = a 64-bit multiply on the
measured path; a counter read is 1-3 instructions). The seam mirrors the existing
`arch_clock_now` split exactly — because the M0/M0+ (armv6-m) genuinely lacks the
debug/trace hardware the M3/M4 (armv7-m) have (no DWT), the *clock* already
fractured along this line at M1. So:

| arch / chip | source | width | rate | notes |
|---|---|---|---|---|
| armv7m (silicon) | DWT `CYCCNT` (enabled by `kickos_armv7m_init`) | 32 | core clk | **uniform arch-layer impl.** Cycle-accurate. |
| armv7m / mps2 (QEMU) | semihosting `SYS_CLOCK` (mps2 overrides — DWT is frozen on QEMU) | 32 | 100 Hz-ish | coarse, monotonic; enables the QEMU **structural** CI gate (§8.4) so the fragile PendSV-tail asm gets automated coverage. |
| armv6m / RP2040 | free-running **1 µs TIMER** (read `TIMERAWL`; same source as `arch_clock_now`) | 32 | 1 MHz | **chip-provided capability**, no ALARM spent, SysTick untouched. Coarse (§ below). |
| armv6m / nRF51 | its TIMER peripheral, or declines | — | — | chip capability; if none → telemetry gated off (§6). |
| sim | `arch_clock_now` → **µs** as u32 (ns/1000) | 32 | 1e6 | µs not ns, so it wraps at ~71 min not 4.29 s (CI runs never alias). Logic/structural only, not a timing benchmark. |
| xtensa / esp32 | `CCOUNT` (RSR) — free-running, independent of the kernel timer | 32 | CPU clk | cycle-accurate. |
| rx / rx72m | the free-running **CMTW** the RX port already runs for `arch_clock_now` (one `CMWCNT` read; CMTW min prescaler is PCLK/8) | 32 | ~PCLK/8 | **not** a "cycle counter" — RXv3 has none. |

**Why RP2040 stays at 1 µs (the CRITICAL-1 resolution).** The first draft put
`arch_trace_now` on SysTick CVR — but SysTick **is** the kernel's tickless one-shot
timer (`arch_armv6m.cc`; re-armed every switch via `ktime_rearm`), so it can't
double as a trace clock. Getting sub-µs would require freeing SysTick and moving the
kernel one-shot to a hardware TIMER ALARM — which quantizes kernel deadlines to 1 µs
and adds the absolute-compare arm-race, i.e. **taxes the scheduler hot path to
benefit cold-path measurement.** Wrong trade. **Decision: keep SysTick as the kernel
one-shot on every board (no rework, uniform timer path preserved); on RP2040
`arch_trace_now` just reads the free-running 1 µs TIMER** (the 4 alarms stay free).
This is exactly what FreeRTOS/NuttX do (periodic SysTick tick + 1 µs TIMER stats).

Consequence, stated honestly: 1 µs on a ~1-5 µs switch is 20-100% per-*sample*
error, and it likely **cannot resolve the M2 MPU-reprogram cost** on an M0+ (may be
sub-µs). So RP2040 gives coarse-but-real operational numbers; **the M2-gating
baseline is defended on XMC/DWT** (cycle-accurate), where the MPU delta is visible.

**Gate mechanism (not a weak symbol):** a per-arch/chip CMake capability var
`KICKOS_HAVE_TRACE_CLOCK` (set in the arch/chip branch) checked at configure →
`FATAL_ERROR` if telemetry is enabled without it; the unresolved strong reference is
the link-time backstop. `arch_trace_now` is **weak in the armv7m layer (DWT)**; a
chip whose DWT is unusable overrides it — **mps2 overrides with semihosting
`SYS_CLOCK`** (the same override it already does for `arch_clock_now`, because
QEMU's DWT is frozen). So QEMU telemetry is *coarse but real* and drives the
structural CI gate (§8.4) — the capability var stays honest per chip.

## 2. Record format

Binary, **little-endian** (all targets LE — asserted). **Fixed length per type**
⇒ self-delimiting. First byte is `type`; then `seq(u16)`; then the payload:

```
type  name            payload (after type, seq)                                    size
0x01  SESSION   magic(u16=0x4B54) ver(u8) arch(u8) ts_bits(u8) clock_hz(u32)
                idle_tid(u16) records_attempted(u32) probe_overhead(u16)
                t_anchor(u64=arch_clock_now)                                        28B
0x02  SWITCH    t(u32) from_tid(u16) to_tid(u16)                                    11B
0x03  SYSCALL_ENTER t(u32) tid(u16) nr(u16)                                         11B
0x04  SYSCALL_EXIT  t(u32) tid(u16) nr(u16)                                         11B
0x08  IRQ_ENTER t(u32) line(u16)                                                     9B
0x09  IRQ_EXIT  t(u32) line(u16)                                                     9B
```

No IDLE event (switch to/from `idle_tid` = idle; CPU% derived host-side). No
OVERFLOW event (loss rides on `seq`). Rules, each closing a named failure mode:

- **(a) Record-atomic.** The ring write checks free space for the *whole* record;
  won't-fit ⇒ drop the **entire** record. Never partial (a half binary record
  desyncs the decoder permanently).
- **(b) Atomic emit under one lock (CRITICAL — review finding).** `seq` assignment,
  the `arch_trace_now` stamp, **and** the ring write happen inside the **same
  `IrqLock`**. Emits come from thread + ISR + the PendSV tail with no atomics on M0;
  taking `seq`/`t` outside the lock reorders/duplicates `seq` across preemption and
  destroys the loss signal. The lock covers counter-read + increment + ≤11 B copy —
  still µs-class. Timestamps may still be non-monotonic across contexts; the decoder
  must not assume global `t` monotonicity (per-pair/per-chain deltas only).
- **(c) Loss via `seq` gap.** `seq` is assigned per emit-*attempt* (dropped ones
  too); delivered `seq` jumping by k ⇒ k dropped. Zero ring cost, so loss reporting
  can't itself be an overflow victim. Backstop for a burst >65535 between two
  *delivered* records: SESSION carries the **absolute u32 `records_attempted`** →
  every delivered SESSION re-anchors seq absolutely (closes the u16-wrap hole even
  if capture ends mid-burst). Per-record `seq` stays u16.
- **(d) Wrap + idle anchor.** `ts_bits` gives the counter width (24/32) so the host
  takes deltas mod 2^ts_bits, not a fixed 2^32. SESSION carries a coarse 64-bit
  `t_anchor` (`arch_clock_now`) tying the trace domain to real time. SESSION is
  emitted on **both** a record-count trigger (every 256) **and** a low-rate timer
  heartbeat (bounds wrap during a long idle, when no records flow). The decoder
  flags any delta > ½ wrap as suspect rather than trusting it.
- **(e) Every EXIT/record self-identifies.** `SYSCALL_ENTER/EXIT` carry both `tid`
  and `nr`; `IRQ_ENTER/EXIT` carry `line`. Pairing is by nesting (IRQ = strictly
  LIFO on Cortex-M; tail-chaining sequences, doesn't nest) and by thread (syscalls
  interleave *by thread* — the host attributes via `tid`, no longer dependent on the
  SWITCH stream surviving). rule (e) is for **fast re-anchoring, not continuation**.
- **(f) Loss invalidation policy (CRITICAL — review finding).** A `seq` gap
  **poisons all open ENTER/EXIT pairs and excludes the enclosing inter-anchor span
  from CPU%/latency aggregates** — the decoder must NOT keep pairing across a gap
  (else a dropped SWITCH pair silently inflates CPU%, a dropped EXIT emits a giant
  bogus duration into p99). The SWITCH `from == previous.to` chain is a per-record
  integrity check the decoder exploits to detect a dropped SWITCH.

**Thread identity.** Threads have no numeric id today (TCB pointer + `name`). 2a adds
a `uint16_t id` to the TCB via a **per-`Kernel`** monotonic counter at
`thread_create` (per-Kernel so the multi-instance sim's traces stay independent).
`kmain` creates **idle first** ⇒ idle is **tid 0**, asserted in `kmain`; **`0xFFFF`
= "no thread"** sentinel (e.g. the boot switch's `from`). The counter skips
`0`/`0xFFFF` on wrap.

**Framing = self-delimiting + record-atomic, NOT byte-stuffing.** RTT is lossless
and record-atomic over a NoBlockSkip ring, so the stream is always whole-record-
aligned (host reads from the control block's read offset). No COBS on RTT — it buys
corruption-recovery RTT doesn't need, and its encode cost lands on the measured
path. Framing robustness is a **per-sink** property: a future byte-lossy sink (UART
binary, socket) wraps records in COBS *at that sink*.

## 3. Frontend — layered so golden vectors are deterministic (review finding H5)

Two layers:
- **Pure encoders** `encode_<event>(buf, seq, t, payload...)` — no globals, no
  clock read. The CI golden-vector test drives these with scripted `seq`/`t` → the
  byte layout is locked deterministically.
- **Thin `ktrace_*` wrappers** (`kernel/include/kickos/ktrace.h`) — take the lock,
  assign `seq`, read `arch_trace_now`, call the encoder, ring-write (rule (b)).
  **Empty inline when `KICKOS_TELEMETRY` off** ⇒ literal zero cost.

Hook points (traced to real code), each gated so *off* is truly free — including the
asm/stash (review finding M7: `#if KICKOS_TELEMETRY` in `switch.S` and `switch_to`):

- **Context switch — snapshot at the physical swap, NEVER re-read shared state
  (review findings: PendSV `to` race + sim deferred-path coalescing).** The record
  must reflect what *actually swapped*, not what `switch_to` last decided (PendSV
  coalesces N decisions→1; SysTick can preempt PendSV and rewrite `g_arch_next`).
  **Rule: at the one physical swap point, capture BOTH the outgoing and incoming
  context pointers into locals, and emit `kickos_trace_switch_done(from,to)` from
  THOSE** (arch→kernel via the RESCAN group) — never re-read `g_arch_next` or a
  `switch_to` stash afterward. `tid` lives at a static-assert'd offset in
  `arch_context`.
  - **armv7m/armv6m** (`switch.S`): PendSV spills the outgoing ctx* at entry AND
    keeps the incoming ctx* it actually loaded; the tail emits from those two
    spilled values. Superseded decisions emit no record.
  - **sim** (`sim.cc`, three physical-swap sites): the synchronous `arch_switch`
    branch (`f`→next), the deferred `isr_frame_leave` (`interrupted`→next), and the
    `makecontext` entry trampoline (first switch-in). Each writes the
    *physically-outgoing* tid at its own swap site — a per-context consume-once
    "switched-in-by" field is cleanest — NOT the shared `switch_to` stash. Create
    ucontexts with the IRQ signals **blocked**, unblocking only after the
    trampoline's emit, so a signal can't drive a second swap mid-hook.
  - **first switch** (`sched::start`→`arch_start`, bypasses `switch_to`):
    `from = 0xFFFF`.
  - PendSV-tail C-callback constraints: save/restore the incoming `EXC_RETURN`
    (`lr`) with 8-byte MSP alignment; `-mgeneral-regs-only` on the emit path (no FP
    mid exception-return); on armv6m place the call before the `EXC_RETURN`
    reconstruction.
- **Syscall enter/exit.** `syscall_dispatch` (`syscall.cc:167`, many `return`s → an
  exit wrapper): `SYSCALL_ENTER{t,tid,nr}` / `SYSCALL_EXIT{t,tid,nr}`. For a
  *blocking* syscall the ENTER→EXIT delta is block duration, not dispatch cost — the
  host segments by intervening SWITCHes.
- **IRQ enter/exit.** `IRQ_ENTER{line}`/`IRQ_EXIT{line}` around `kickos_isr_irq`,
  **and around `kickos_isr_timer`** with a reserved pseudo-line (finding M7/L14 — the
  tick ISR bypasses `kickos_isr_irq` yet is the heaviest recurring ISR; unmeasured
  it invisibly inflates every interval it preempts). `arch_in_isr` is IPSR-derived on
  ARM (a boolean, not a depth counter — LIFO nesting still holds); the RX port uses a
  software depth counter per its spike.
- **Idle / CPU% — no hook.** Derived from SWITCH ± `idle_tid`. Honest bias
  (finding L8): ISR time while idle is booked as idle → CPU% understated on a
  near-idle system; the host can subtract it using the IRQ stream. CPU% is defined as
  valid only within heartbeat-bounded windows.
- **Fault/NMI must never call `ktrace_*`** — they preempt BASEPRI/PRIMASK and would
  tear the ring (except the deliberate flight-recorder marker, §12, which is torn-
  tolerated on a dying system).

## 4. Sink — RTT channel 1, record-atomic

`lib/rtt.cc` gains a binary **channel 1** (`up[2]`, NoBlockSkip) and a record-atomic
writer `kickos_rtt_write_record_ch1(rec, n)` returning false (caller counts a drop)
if the whole record won't fit — never partial. Runs under the same `IrqLock` as
`seq`/stamp (rule (b)). **Publish barrier (review finding M9):** `lib/` is a leaf and
cannot include an arch header, so the ordering macro `KICKOS_RTT_PUBLISH_BARRIER`
(default compiler barrier; it orders ch0 text too, hence the name) is `#ifndef`'d in
`rtt.h` and **overridden via a CMake-injected compile definition** from the
arch/toolchain branch — ESP32 → `MEMW` (the probe is another bus master), RX →
compiler-only suffices. The two in-flight ports then drop in a fence with **no edit
to `lib/rtt.cc`**. Cached-core caveat (ESP32/RX72M): the ch1 buffer must be uncached
(or cache-maintained), which also makes those ports' per-emit cost differ — factored
by the per-port probe calibration (§7).

## 5. Instance scoping

Telemetry counters (`seq`, `records_attempted`) live in **`Kernel`** (the multi-
instance sim mustn't share them); the from/to `tid` stash is `Kernel` state too.
There is **no on-target CPU% accumulator** (host-derived — corrects the earlier doc
drift). `arch_trace_now` is a true global (one clock per core). The RTT block is a
process-global `_SEGGER_RTT`, so **2a telemetry on the sim is single-instance-only**
(stated constraint; per-instance sinks are later). `g_idle_tcb`/`g_root_tcb` are
still file-static, so "each Kernel's idle = tid 0" fully holds only after that Later
per-instance work; single-instance is correct now.

## 6. Config

`KICKOS_TELEMETRY = off | rtt` (cache STRING, per-board default `off`). `off` ⇒ every
`ktrace_*` empty inline, the asm hook `#if`-compiled out, `arch_trace_now`
unreferenced → literal zero cost. `rtt` requires `KICKOS_HAVE_TRACE_CLOCK` (§1) AND
`KICKOS_CONSOLE ∈ {rtt, both}`, else configure `FATAL_ERROR`. Slots into the existing
root-`CMakeLists.txt` per-board pattern (the `KICKOS_CONSOLE` precedent); the CI
preset must set `KICKOS_CONSOLE=both`. Growth path (NuttX-style, §"prior art"):
per-event-class enables (switch / syscall / irq) as booleans later.

## 7. Host decoder (`tools/kicktrace.py`)

Pure bytes-in → CSV/summary (CI-testable, no probe). Input: `JLinkRTTClient` ch1, or
OpenOCD `rtt` (block addr/size from the ELF symbol `_SEGGER_RTT`), or a saved dump.

- **decode / sync (scan is the fallback, not the norm):**
  - **Primary — no scan.** Record-atomic + NoBlockSkip ⇒ the read offset is always a
    boundary; loop `read type → fixed length → consume`, committing only whole records.
  - **Continuous check.** `type` known + `seq` advances; else re-sync.
  - **Cold attach / re-sync — the two-anchor lock.** Scan for the SESSION magic →
    candidate; **parse forward by type→length**: a true boundary lands *exactly* on
    the **next** magic with `seq` contiguous (gaps = drops accounted); a magic-in-
    payload false hit overshoots/undershoots or hits an invalid type → advance one
    byte, retry. Two consistent anchors ⇒ locked. Works for both cadence and
    heartbeat SESSIONs; worst-case ≤ ~2× anchor spacing. (A dump shorter than one
    SESSION cadence has no header → undecodable; state the minimum-capture rule.)
  - **loss:** `seq` gaps per record, cross-checked against `SESSION.records_attempted`.
- **outputs:** CSV; summaries — switch latency (min/mean/p50/p99), syscall latency by
  `nr`, ISR duration by `line`, **CPU%** (per heartbeat window, ISR-during-idle
  subtracted); **Chrome-trace / Perfetto JSON** for a flamegraph timeline. SystemView-
  compatible emission is an optional later add-on.
- **probe-overhead subtraction (§ perturbation):** the host subtracts the SESSION-
  published probe overhead from published absolute latencies.

## 8. CI gates (sim, no hardware)

1. **Golden-vector round-trip** — drive the *pure encoders* (§3) with scripted
   `seq`/`t` into a RAM buffer, decode with `kicktrace.py`, assert exact events +
   computed CPU%/latency. Locks byte layout + endianness + framing.
2. **Ring wrap / full-drop** — overfill ch1; assert record-atomic drop (no partial),
   correct `seq` gaps, and `records_attempted`/`dropped` accounting (no OVERFLOW
   record). (KICKOS_CONSOLE=both on this preset.)
3. **End-to-end sim run** — real ping-pong with telemetry on, decoded by
   `kicktrace.py`, **timing-agnostic structural** assertions: SWITCH `from==prev.to`
   chain intact, ENTER/EXIT pair, no seq gaps. Catches the sim hook-site bugs gate 1
   (scripted) cannot. Must include a **deliberate multi-wake-in-one-ISR** case (one
   timer/IRQ handler that wakes ≥2 threads → coalesced switch) — else the coalescing
   path is never exercised. Sim writes the ch1 ring to a file at `arch_shutdown` for
   `kicktrace.py` input (no probe on the host sim).
4. **QEMU structural run (armv7m tail asm coverage)** — the same structural checks on
   the `qemu` board, using mps2's semihosting trace clock (§1). This is the ONLY
   automated coverage of the fragile PendSV-tail C-callback (EXC_RETURN save, MSP
   alignment, `-mgeneral-regs-only`); without it that asm is validated only by the
   manual XMC step.

## 9. Perturbation (stated, measured, partly correctable)

Each emit adds ~40-80 cyc on armv7m (DSB+ISB in `IrqLock`) and ~5-10 µs on the
RP2040 at 12 MHz (armv6m PRIMASK masks everything → this directly inflates the IRQ
latency it reports). Mitigations: (a) stamp position per event type (ENTER stamps
last-before-write, EXIT/SWITCH first) to minimize included probe cost; (b) measure
probe overhead at telemetry init (back-to-back `arch_trace_now` + a null pair) and
publish it in SESSION for host subtraction; (c) the saving grace — for the **M2
MPU A/B comparison the constant probe cost cancels in the delta**, so even
uncorrected the baseline-vs-MPU comparison survives (absolute baselines need (b)).

## 10. Phasing

- **2a (this step):** TCB `uint16_t id` + tid-0 convention; `arch_trace_now` (armv7m
  DWT + RP2040/armv6m 1 µs TIMER + sim); the layered encoder + record format;
  `ktrace_*` + SWITCH (PendSV-truth hook), SYSCALL, IRQ (incl. the timer ISR);
  RTT ch1 record-atomic sink + the CMake-injected publish barrier; `Kernel`-scoped
  counters; `KICKOS_TELEMETRY` + `KICKOS_HAVE_TRACE_CLOCK`; the three sim CI gates;
  `kicktrace.py`. **Manual HW measure on XMC (cycle-accurate baseline — the M2 gate)
  + RP2040 (coarse operational)**; defend the baseline.
- **2b:** per-event-class enable flags; Perfetto output; SystemView option; per-arch
  `arch_trace_now` for the RX/ESP32 ports as they merge; the flight-recorder-on-fault
  (§12) if not folded into 2a.

## 11. Resolved decisions

1. `seq` = **u16** per record + **u32 `records_attempted`** absolute anchor in SESSION.
2. SESSION re-emit on **both** every-256-records **and** a timer heartbeat (idle wrap).
3. Idle/CPU% **derived** from SWITCH + tid-0 (no hook).
4. IRQ events **in 2a** (incl. the timer ISR); per-line enable is 2b.
5. RP2040 clock: **SysTick stays the kernel one-shot; trace = 1 µs TIMER read**; the
   M2-gating cycle-accurate baseline is **XMC/DWT**. (No RP2040 timer re-arch.)
6. Loss: seq-gap + `records_attempted`; **gap poisons open pairs + excludes the span**.
7. Emit is **atomic under one `IrqLock`** (seq + stamp + ring write).

## 12. Kernel-aware debugging (the second half of the charter)

The same instrumented stream gives, at increasing cost:
1. **Live scheduling view (free with 2a).** The SWITCH/SYSCALL/IRQ stream *is* a
   SystemView/Tracealyzer-style timeline (scheduling, ISR nesting, CPU%) without
   halting. SystemView-compatible emission borrows SEGGER's viewer; `kicktrace.py`
   → Perfetto is ours.
2. **Flight recorder / post-mortem (high value, low cost — like NuttX `noteram`).**
   The trace ring lives in target RAM; on a halt/fault a debugger reads the last N
   records → the exact switch/syscall/IRQ sequence that led to the crash, no live
   host. Only add: the fault handler drops a torn-tolerated marker record (and may
   force-flush). Candidate 2a-adjacent win.
3. **On-halt RTOS awareness (follow-on).** A GDB Python OS-awareness script or a
   J-Link RTOS plugin enumerates threads/states/per-thread backtraces on halt by
   reading a well-known thread-list anchor + stable TCB offsets — reusing the same
   `id`/tid-0 identity model 2a adds.

## 13. Open decisions to confirm

1. **Flight-recorder marker in 2a or 2b?** The RAM ring already exists; adding a
   fault-handler marker (§12.2) is small and turns telemetry into a black-box
   recorder immediately. Proposed: **2a**.
2. **Heartbeat source.** On MCUs the kernel one-shot's RVR clamp already forces a
   wake at least every ~0.12 s @144 MHz / ~1.4 s @12 MHz (< the DWT 29.8 s / TIMER
   71 min wraps) — state that cadence explicitly rather than lean on it by accident.
   The **sim has no periodic wake** (`timer_settime` is absolute), which is why the
   sim trace clock is µs (§1) — wraps at 71 min, so a CI run never aliases without a
   heartbeat. Proposed: MCU heartbeat rides the clamped tick; sim relies on the µs
   clock. Confirm.

## 14. Fixed constants + edge cases (pin before coding)

- **Reserved `line` ids:** `0xFFFE` = the timer-ISR pseudo-line (distinct from real
  NVIC lines and from the `0xFFFF` no-thread tid).
- **arch id (SESSION.arch, u8):** `0=sim, 1=armv7m, 2=armv6m, 3=xtensa, 4=rx`.
- **ch1 ring size:** default 1 KiB (`KICKOS_TELEMETRY_BUF`, per-board overridable).
- **No-return syscalls** (`KOS_SYS_exit`, `syscall.cc`): emit `SYSCALL_ENTER` but no
  `SYSCALL_EXIT` — the decoder closes that pair at the next SWITCH away from the tid
  (rule (f)'s per-context stack already handles an unclosed ENTER at a boundary).
- **SESSION `ver`:** starts at 1; the `probe_overhead` field (§9) is part of v1.
