<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS telemetry / kernel-observability

> Reference (code-synced): the telemetry subsystem's record/wire format,
> frontend/backend split, decoder contract, and CI gates. Stays in sync with the
> code (`kernel/ktrace/`, `kernel/include/kickos/ktrace.h`,
> `include/kickos/trace/record.h`, `lib/rtt.cc`, `tools/kicktrace.py`); on any
> disagreement the code wins and this page is the bug. Stable labels the code
> comments cite (do not renumber): **deliverable 3** = record format + pure
> encoders (section 2, `include/kickos/trace/record.h`); **deliverable 4** = the
> emit frontend (section 3, `kernel/include/kickos/ktrace.h`); **deliverable 5**
> = the instance-scoped counters (section 5, `kernel/include/kickos/instance.h`);
> **deliverable 8** = the host decoder (section 7, `tools/kicktrace.py`);
> **CI gates 1-4** = section 8; **G6** = the publish-barrier injection
> (section 4, root `CMakeLists.txt`).

The architecture: a frontend/backend split (pure encoders under thin locked emit
wrappers), a record-atomic binary sink on RTT channel 1, a non-perturbation rule
(nothing heavy on the measured path; telemetry off = zero cost), and
completion-not-decision stamping (SWITCH records reflect the physical swap).

Charter note: telemetry is **kernel observability -- benchmarking *and* debugging
from one instrumented stream**, not benchmarking alone (see section 12).

## Why telemetry precedes M2

M2 reprograms the MPU on every context switch-in (`sched::switch_to` already calls
the no-op `arch_mpu_apply`). Measuring the MPU's per-switch cost *after* it lands is
measuring blind. So telemetry captures switch / syscall / IRQ latency and CPU%
while the switch path is still bare (privilege+SVC only), and defends a baseline
before M2 stacks the MPU on top. That baseline is defended on **XMC4800
(`xmc4800-relax`, armv7m / DWT, cycle-accurate)** -- see section 1 for why not
RP2040. Per-board capture is `tools/telemetry-record.sh` (JLinkRTTLogger, channel
1); the pre-MPU baselines are an M2-readiness item (`docs/m2-readiness.md`
section 3).

## Prior art (NuttX, FreeRTOS -- grounded in the local trees, credited not ranked)

Both converge on the exact shape here, which validates it:
- **FreeRTOS**: empty **trace-hook macros** gated by `configUSE_TRACE_FACILITY`
  (`traceTASK_SWITCHED_IN/OUT` at `tasks.c:3767`, `traceISR_ENTER/EXIT`), `#define`d
  to a recorder -> our compile-out `ktrace_*`. CPU% via `configGENERATE_RUN_TIME_STATS`
  + a user hi-res counter `portGET_RUN_TIME_COUNTER_VALUE` -> our `arch_trace_now`.
  Recorders: Percepio Tracealyzer (snapshot RAM-ring or streaming) + SEGGER SystemView.
- **NuttX**: `CONFIG_SCHED_INSTRUMENTATION` with **per-event-class sub-flags**
  (`_SWITCH`, `_SYSCALL`, `_IRQHANDLER`, `_PREEMPTION`, `_HEAP`, `_FILTER`); kernel
  calls `sched_note_*` ("notes") into a **note-driver** abstraction with many
  backends: `noteram_driver` (RAM ring at `/dev/note` = flight recorder),
  `notesnap`, `notestream`, `noterpmsg` (cross-core), `drivers/segger/note_{rtt,
  sysview}.c`. CPU% via `CONFIG_SCHED_CPULOAD`.

Borrowed here: (a) NuttX's **per-event-class enable** as the config growth path
(section 6); (b) `noteram` = the **flight-recorder-on-halt** idea (section 12); (c) a
**Chrome-trace** decoder output (NuttX emits CTF/Perfetto) -- free, powerful
viewers (section 7). Deliberate divergence: FreeRTOS accumulates CPU% **on the
target** (`portGET_RUN_TIME`); KickOS derives it **host-side** from the SWITCH
stream (leaner, non-perturbing) -- section 3.

---

## 1. `arch_trace_now()` -- the cycle-probe seam (armv7m-uniform / armv6m-chip-capability)

Declared in `arch/include/kickos/arch/arch.h`, next to `arch_clock_now`:

```c
// A dedicated high-resolution monotonic counter for telemetry timestamps: the
// ns arch_clock_now is too coarse to time a context switch (~1-5 us). u32 by
// design (wraps; the decoder reconstructs absolute time from the SESSION
// anchors). A target that has no such source does NOT define
// KICKOS_HAVE_TRACE_CLOCK and cannot enable telemetry (build-time FATAL).
uint32_t arch_trace_now(void);
```

**Raw counter, host converts** (in-target ns would be a 64-bit multiply on the
measured path; a counter read is 1-3 instructions). The seam mirrors the existing
`arch_clock_now` split exactly -- the M0/M0+ (armv6-m) genuinely lacks the
debug/trace hardware the M3/M4 (armv7-m) have (no DWT), so the *clock* already
fractured along this line at M1. The implementations:

| arch / chip | source | width | rate | notes |
|---|---|---|---|---|
| armv7m (silicon) | DWT `CYCCNT` (enabled by `kickos_armv7m_init`) | 32 | core clk | **weak, uniform arch-layer impl** (`arch/arm/armv7m/arch_armv7m.cc`). Cycle-accurate. |
| armv7m / mps2 (QEMU, board `qemu`) | semihosting `SYS_CLOCK` scaled to us (`chip_mps2.cc` overrides -- DWT is frozen on QEMU) | 32 | 1 MHz nominal, ~10 ms resolution | coarse, monotonic; drives the QEMU **structural** CI gate 4 (section 8) so the fragile PendSV-tail asm gets automated coverage. |
| armv6m / RP2040 (`picopi`) | free-running **1 us TIMER** (one `TIMERAWL` read; same source as `arch_clock_now`) | 32 | 1 MHz | **chip-provided capability** (`chip_rp2040.cc`), no ALARM spent, SysTick untouched. Coarse (below). |
| armv6m / nRF51 (`microbit`, a QEMU vehicle) | semihosting `SYS_CLOCK` scaled to us (`chip_nrf51.cc`) | 32 | 1 MHz nominal, ~10 ms resolution | structural coverage only, like mps2. |
| sim | `arch_clock_now` -> **us** as u32 (ns/1000) | 32 | 1 MHz | us not ns, so it wraps at ~71 min not 4.29 s (CI runs never alias). Logic/structural only, not a timing benchmark. |
| xtensa / esp32 | `CCOUNT` (RSR) -- free-running, independent of the kernel timer (`arch_xtensa.cc`) | 32 | CPU clk | cycle-accurate. |
| rx / rx72m | the free-running **CMTW1** the RX port already runs for `arch_clock_now` (one `CMWCNT` read; CMTW min prescaler is PCLK/8) (`arch_rxv3.cc`) | 32 | ~PCLK/8 | **not** a "cycle counter" -- RXv3 has none. |
| riscv / rv32imac | `rdcycle` CSR, low 32 bits (`arch_rv32imac.cc`; `mcounteren.CY` is opened in `kickos_rv32_init` so U-mode can read it too) | 32 | CPU clk | cycle-accurate. |

**Why RP2040 stays at 1 us.** SysTick **is** the kernel's tickless one-shot timer
on this port (`arch_armv6m.cc`; re-armed every switch via `ktime_rearm`), so it
cannot double as a trace clock. Getting sub-us would require freeing SysTick and
moving the kernel one-shot to a hardware TIMER ALARM -- which quantizes kernel
deadlines to 1 us and adds an absolute-compare arm-race, i.e. taxes the scheduler
hot path to benefit cold-path measurement. Wrong trade. So SysTick stays the
kernel one-shot on every board (uniform timer path), and on RP2040
`arch_trace_now` reads the free-running 1 us TIMER (the 4 alarms stay free) --
exactly the FreeRTOS/NuttX shape (SysTick tick + 1 us TIMER stats).

Consequence, stated honestly: 1 us on a ~1-5 us switch is 20-100% per-*sample*
error, and it likely cannot resolve the M2 MPU-reprogram cost on an M0+ (may be
sub-us). RP2040 gives coarse-but-real operational numbers; **the M2-gating
baseline is defended on XMC/DWT** (cycle-accurate), where the MPU delta is visible.

**Gate mechanism (not a weak symbol):** the CMake capability var
`KICKOS_HAVE_TRACE_CLOCK` (root `CMakeLists.txt`; defaulted per arch/chip,
overridable by a board/chip/preset) is checked at configure ->
`FATAL_ERROR` if `KICKOS_TELEMETRY=rtt` without it; an unresolved strong reference
is the link-time backstop. `arch_trace_now` is **weak in the armv7m layer (DWT)**;
a chip whose DWT is unusable overrides it -- **mps2 overrides with semihosting
`SYS_CLOCK`** (the same override it already does for `arch_clock_now`, because
QEMU's DWT is frozen). So QEMU telemetry is *coarse but real* and drives the
structural CI gate 4 (section 8) -- the capability var stays honest per chip.

## 2. Record format

Binary, **little-endian** (all targets LE). **Fixed length per type** =>
self-delimiting (the decoder maps type -> length, never guesses). Every record
starts with the same 7-byte prefix: `type(u8) seq(u16) t(u32)`, where `t` is
`arch_trace_now()` at emit. Type tags are kept nonzero so a zeroed buffer never
decodes as a valid record. Source of truth: `include/kickos/trace/record.h`.

```
type  name           payload (after the 7-byte prefix)                        size
 1    SESSION        magic(u32='KTRC'=0x4B545243) ver(u8=1) arch(u8)
                     ts_bits(u8=32) probe_overhead(u16)
                     records_attempted(u32) t_anchor(u64=arch_clock_now)       28B
 2    SWITCH         from_tid(u16) to_tid(u16)                                 11B
 3    SYSCALL_ENTER  tid(u16) nr(u16)                                          11B
 4    SYSCALL_EXIT   tid(u16) nr(u16)                                          11B
 5    IRQ_ENTER      line(u16)                                                  9B
 6    IRQ_EXIT       line(u16)                                                  9B
```

No IDLE event (a switch to/from tid 0 = idle; CPU% is derived host-side). No
OVERFLOW event (loss rides on `seq`). No clock-rate field (the rate is derived
from the SESSION anchors, or given as `--clock-hz`). Rules, each closing a named
failure mode:

- **(a) Record-atomic.** The ring write checks free space for the *whole* record;
  won't-fit => drop the **entire** record. Never partial (a half binary record
  desyncs the decoder permanently).
- **(b) Atomic emit under one lock.** `seq` assignment, the `arch_trace_now`
  stamp, **and** the ring write happen inside the **same `IrqLock`**
  (`kernel/include/kickos/ktrace.h`). Emits come from thread + ISR + the PendSV
  tail with no atomics on M0; taking `seq`/`t` outside the lock would
  reorder/duplicate `seq` across preemption and destroy the loss signal. The lock
  covers counter-read + increment + <=28 B copy -- still us-class. Timestamps may
  still be non-monotonic across contexts; the decoder does not assume global `t`
  monotonicity beyond wrap reconstruction (per-pair/per-chain deltas only).
- **(c) Loss via `seq` gap.** `seq` is assigned per emit-*attempt* (dropped
  records consume one too); delivered `seq` jumping by k => k dropped. Zero ring
  cost, so loss reporting cannot itself be an overflow victim. Backstop for a
  burst >65535 between two *delivered* records: SESSION carries the **absolute
  u32 `records_attempted`** -> every delivered SESSION re-anchors the count
  absolutely (closes the u16-wrap hole even if capture ends mid-burst).
  Per-record `seq` stays u16.
- **(d) Wrap + anchors.** `ts_bits` publishes the counter width (32 on every
  current backend) so the host takes deltas mod 2^ts_bits. SESSION is emitted
  **twice per run**: an opening SESSION from `ktrace_init` (called in `kmain`)
  and a closing SESSION from `kickos_trace_final_session` (called by
  `arch_shutdown` on the flushing backends). Each pairs a u32 trace stamp `t`
  with a u64 `arch_clock_now` anchor `t_anchor`; the two anchors define the
  tick->ns rate and tie the trace domain to real time (section 7). The decoder's
  unwrap assumes at most one wrap between consecutive records; in practice the
  clamped SysTick one-shot bounds record gaps far below the wrap period on MCUs
  (the timer ISR is itself traced), and the sim's us clock wraps at ~71 min.
- **(e) Every EXIT/record self-identifies.** `SYSCALL_ENTER/EXIT` carry both `tid`
  and `nr`; `IRQ_ENTER/EXIT` carry `line`. Pairing is by nesting (IRQ = strictly
  LIFO on Cortex-M; tail-chaining sequences, doesn't nest) and by thread (syscalls
  interleave *by thread* -- the host attributes via `tid`, not dependent on the
  SWITCH stream surviving). Rule (e) is for **fast re-anchoring, not continuation**.
- **(f) Loss invalidation policy.** A `seq` gap **poisons all open ENTER/EXIT
  pairs and excludes the enclosing span from CPU%/latency aggregates** -- the
  decoder does NOT keep pairing across a gap (else a dropped SWITCH pair silently
  inflates CPU%, a dropped EXIT emits a giant bogus duration into the tail
  percentiles). The SWITCH `from == previous.to` chain is a per-record integrity
  check the decoder exploits to detect a dropped SWITCH.

**Thread identity.** Every TCB carries a `uint16_t id`
(`kernel/include/kickos/thread.h`), assigned in `thread_create` from a
**per-`Kernel`** monotonic counter (per-Kernel so the multi-instance sim's traces
stay independent). `kmain` creates **idle first** => idle is **tid 0**, asserted
in `kmain`; **`0xFFFF` = "no thread"** sentinel (e.g. the boot switch's `from`).
The counter skips `0`/`0xFFFF` on wrap. `thread_create` also stamps the id into
the saved arch context (`arch_trace_stamp_id`) so the switch path can emit it
from the physically-swapped contexts (section 3).

**Framing = self-delimiting + record-atomic, NOT byte-stuffing.** RTT is lossless
and record-atomic over a NoBlockSkip ring, so the stream is always whole-record-
aligned (the host reads from the control block's read offset). No COBS on RTT --
it buys corruption-recovery RTT doesn't need, and its encode cost lands on the
measured path. Framing robustness is a **per-sink** property: a future byte-lossy
sink (UART binary, socket) wraps records in COBS *at that sink*.

## 3. Frontend -- layered so golden vectors are deterministic

Two layers:
- **Pure encoders** `encode_<event>(buf, seq, t, payload...)`
  (`include/kickos/trace/record.h`) -- header-only, no globals, no clock read.
  The CI golden-vector gate drives these with scripted `seq`/`t` -> the byte
  layout is locked deterministically.
- **Thin `ktrace_*` wrappers** (`kernel/include/kickos/ktrace.h`) -- take the
  lock, assign `seq`, read `arch_trace_now`, call the encoder, ring-write
  (rule (b)). **Empty inline when `KICKOS_TELEMETRY` is off** => literal zero
  cost, no symbols, hot paths byte-unchanged.

Hook points, each gated so *off* is truly free -- including the asm
(`#if KICKOS_TELEMETRY` in every port's `switch.S` and in the kernel C hooks):

- **Context switch -- snapshot at the physical swap, NEVER re-read shared
  state.** The record must reflect what *actually swapped*, not what `switch_to`
  last decided (PendSV coalesces N decisions -> 1; SysTick can preempt PendSV and
  rewrite `g_arch_next`). **Rule: at the one physical swap point, capture BOTH
  the outgoing and incoming context pointers into locals, and call
  `kickos_trace_switch_done(from_tid, to_tid)` from THOSE** (arch -> kernel, in
  the RESCAN group of `arch.h`) -- never re-read `g_arch_next` or scheduler state
  afterward. The tid lives in the arch context (`trace_tid`, telemetry-only,
  written by `arch_trace_stamp_id`) at a static-assert'd offset the asm reads.
  - **armv7m/armv6m** (`switch.S`): PendSV spills the outgoing ctx* at entry AND
    keeps the incoming ctx* it actually loaded; the tail emits from those two
    values. Superseded decisions emit no record. Tail constraints, live in the
    asm: MSP kept 8-byte aligned across the `bl`; on armv7m the incoming
    `EXC_RETURN` (`lr`) is saved/restored around the call; on armv6m the call
    sits before the `EXC_RETURN` reconstruction (so `lr` needs no save); the C
    side (`kernel/ktrace/ktrace.cc`) is built `-mgeneral-regs-only` so the emit
    never touches FP state mid exception-return.
  - **xtensa / rx / riscv** (`lx6/switch.S` + `esp32/startup.S`,
    `rxv3/switch.S`, `rv32imac/switch.S`): the same pattern at each port's
    physical swap site(s).
  - **sim** (`sim.cc`): a per-instance consume-once hand-off.
    `trace_switch_arm(from_tid)` is armed just before every ucontext swap
    (synchronous `arch_switch`, deferred `isr_frame_leave`); whichever context
    RESUMES -- right after its `swapcontext`, or a new thread at the trampoline --
    consumes it and emits `{from -> current}`. New ucontexts are created with the
    IRQ signals **blocked**, unblocked only after the trampoline's emit, so a
    signal can't drive a second swap mid-hook.
  - **first switch** (`sched::start` -> `arch_start`, bypasses `switch_to`):
    `from = 0xFFFF`.
- **Syscall enter/exit.** `syscall_dispatch` (`kernel/syscall/syscall.cc`) wraps
  the dispatch in an RAII bracket: `SYSCALL_ENTER{t,tid,nr}` on entry,
  `SYSCALL_EXIT{t,tid,nr}` from the destructor on every ordinary return path.
  `KOS_SYS_EXIT` switches away permanently inside the dispatch, so it is
  recorded ENTER-only (the decoder tolerates it, section 7). For a *blocking*
  syscall the ENTER->EXIT delta is block duration, not dispatch cost -- the
  decoder segments on-CPU overhead by intervening SWITCHes.
- **IRQ enter/exit.** `IRQ_ENTER{line}`/`IRQ_EXIT{line}` bracket the handler in
  `kickos_isr_irq` (`kernel/irq/irq.cc`), **and bracket `kickos_isr_timer`**
  (`kernel/time/time.cc`) with the reserved pseudo-line `0xFFFE` -- the tick ISR
  bypasses `kickos_isr_irq` yet is the heaviest recurring ISR; unmeasured it
  would invisibly inflate every interval it preempts. `arch_in_isr` is
  IPSR-derived on ARM (a boolean, not a depth counter -- LIFO nesting still
  holds); the RX, xtensa and riscv ports use a software depth flag/counter.
- **Idle / CPU% -- no hook.** Derived host-side from SWITCH +/- tid 0. Honest
  bias: ISR time while idle is booked as idle -> CPU% understated on a near-idle
  system; `kicktrace.py` subtracts it using the IRQ stream (section 7).
- **Fault/NMI never call `ktrace_*`** -- they preempt BASEPRI/PRIMASK and would
  tear the ring. (A deliberate torn-tolerated flight-recorder marker is future
  work, section 12.)

## 4. Sink -- RTT channel 1, record-atomic

`lib/rtt.cc` publishes two up channels: 0 = console text, 1 = binary telemetry
(named "Telemetry", NoBlockSkip). The record-atomic writer
`kickos_rtt_write_record_ch1(rec, n)` returns 0 (the caller counts a drop) if the
whole record won't fit -- never partial -- and commits the ring's write offset
once, after the payload, so the record appears atomically. It runs under the same
`IrqLock` as `seq`/stamp (rule (b)). `kickos_rtt_ch1_drain` reads the ring back
out for the file-flush backends (section 8). **Publish barrier (G6):** `lib/` is
a leaf and cannot include an arch header, so the ordering macro
`KICKOS_RTT_PUBLISH_BARRIER` (default: compiler-only barrier; it orders ch0 text
too, hence the name) is `#ifndef`'d in `rtt.h` and **overridable via a
CMake-injected compile definition** (`KICKOS_RTT_PUBLISH_BARRIER_DEF`, root
`CMakeLists.txt`). The in-order M-class M1 targets keep the default; a
weakly-ordered or multi-master core injects its real fence there (ESP32 ->
`MEMW`, the probe is another bus master) with **no edit to `lib/rtt.cc`**.
Cached-core caveat (ESP32/RX72M): the ch1 buffer must be uncached (or
cache-maintained), which also makes those ports' per-emit cost differ -- factored
by the per-port probe calibration (section 9). Ring size: `KICKOS_RTT_CH1_SIZE`,
default 4096 bytes; the sim/CI preset raises it (a host that drains only at
shutdown must hold a whole run).

## 5. Instance scoping

Telemetry counters (`trace_seq`, `trace_records_attempted`, `trace_dropped`,
`trace_probe_overhead`) live in **`Kernel`** (`kernel/include/kickos/instance.h`)
-- the multi-instance sim must not share them. The switch tids ride in the arch
contexts (`arch_trace_stamp_id`), and the sim's consume-once hand-off is
per-`SimInstance`. There is **no on-target CPU% accumulator** (host-derived).
`arch_trace_now` is a true global (one clock per core). The RTT block is a
process-global `_SEGGER_RTT`, so **telemetry on the sim is single-instance-only**
(stated constraint; per-instance sinks are later). `g_idle_tcb`/`g_root_tcb` are
still file-static, so "each Kernel's idle = tid 0" fully holds only after that
later per-instance work; single-instance is correct now.

## 6. Config

`KICKOS_TELEMETRY = off | rtt` (cache STRING, per-board default `off`). `off` =>
every `ktrace_*` an empty inline, the asm hooks `#if`-compiled out,
`arch_trace_now` unreferenced -> literal zero cost. `rtt` requires
`KICKOS_HAVE_TRACE_CLOCK` (section 1) AND `KICKOS_CONSOLE in {rtt, both}`, else
configure `FATAL_ERROR`. The build bakes the SESSION arch id in as
`KICKOS_TRACE_ARCH` (section 14). Ready-made presets: `sim-telem`
(`KICKOS_CONSOLE=both`, `KICKOS_RTT_CH1_SIZE=16384`) and `qemu-telem`
(`KICKOS_CONSOLE=both`), in `cmake/presets/`. Growth path (NuttX-style, section
"prior art"): per-event-class enables (switch / syscall / irq) as booleans later.

## 7. Host decoder (`tools/kicktrace.py`)

Pure bytes-in -> CSV / summary / Chrome-trace JSON (CI-testable, no probe).
Input: a saved ch1 dump (the sim and mps2 backends write one at shutdown), or a
live stream via `--follow` (stdin/fifo, e.g. `JLinkRTTLogger -RTTChannel 1`;
`tools/telemetry-record.sh` wraps the capture per board).

- **decode / sync (scan is the fallback, not the norm):**
  - **Primary -- no scan.** Record-atomic + NoBlockSkip => the stream is
    whole-record-aligned; loop `read type -> fixed length -> consume`.
  - **Batch re-sync.** An unknown type or bad SESSION magic triggers a scan for
    the next SESSION magic (`'KTRC'` at prefix+0, validated by the type byte 7
    before it); parsing resumes there and the skipped bytes are reported.
  - **Live lock (`--follow`).** Alignment is by seq-contiguity: a window of
    consecutive records whose `seq` values chain locks the byte grid -- no
    SESSION marker needed, so it locks onto a mid-stream capture and re-locks
    after any desync. Records print in canonical form as they arrive; time stays
    in raw ticks (the ns model needs both anchors, the last only at shutdown),
    and the aggregate views are batch-only.
  - **Clock model.** The first and last SESSION each pair `t` (trace ticks) with
    `t_anchor` (ns); two anchors define the tick->ns rate and unwrap the u32
    across the span. One anchor still yields a rate (both clocks share an
    origin); zero anchors -> raw ticks. `--clock-hz` overrides with an exact
    rate (e.g. the DWT/core clock) and works with no anchor at all.
  - **loss:** `seq` gaps per record, cross-checked against the closing SESSION's
    `records_attempted` (`decoded + lost == attempted`; skipped without a clean
    shutdown). Rule (f) applies: gaps poison open pairs and exclude their span.
- **outputs:** `--csv` canonical per-record lines (the golden-vector contract);
  `--summary` -- switch counts and per-thread run time, syscall **on-CPU
  overhead** (block time segmented out via SWITCHes) overall and by `nr`,
  syscall span, ISR duration by `line`, wake->switch latency, **CPU%**
  (ISR-during-idle subtracted), stats as min/max/mean/p50/p95; `--chrome`
  Chrome-trace JSON for a timeline (loads in Perfetto / chrome://tracing);
  `--assert-structural` and `--assert-atomic` for the CI gates (section 8).
  SystemView-compatible emission is an optional later add-on.
- **probe-overhead subtraction (section 9):** the host subtracts the
  SESSION-published probe overhead from published absolute latencies.

## 8. CI gates (host-runnable, no probe)

1. **Golden-vector round-trip** (`tests/telemetry/gen_golden.cc` +
   `check_golden.py`, ctest `telemetry_golden`) -- drives the *pure encoders*
   (section 3) with scripted `seq`/`t` into a file, decodes with `kicktrace.py
   --csv`, asserts the exact canonical text. Locks byte layout + endianness +
   framing. Host-only and config-independent: registered on the sim build even
   with telemetry off, so the plain `sim` CI job always runs it.
2. **Ring wrap / full-drop** (`user/apps/tele_flood` + `check_flood.py`, ctest
   `telemetry_ring_wrap`, `sim-telem` preset) -- overfills ch1; asserts
   record-atomic drop (no torn record), contiguous `seq` in the file (the sim
   drains only at shutdown, so drops form a clean tail), and that
   `decoded + dropped == attempted` against the `[ktrace]` counter line printed
   by `kickos_trace_report_counters` (no OVERFLOW record).
3. **End-to-end sim run** (`user/apps/tele_pingpong` + `check_run.py`, ctest
   `telemetry_structural`, `sim-telem` preset) -- real ping-pong with telemetry
   on, decoded by `kicktrace.py --assert-structural`: **timing-agnostic
   structural** assertions -- SWITCH `from==prev.to` chain intact, ENTER/EXIT
   pairs balanced (orphan EXITs flagged; unmatched ENTERs tolerated -- exit(),
   or threads blocked at shutdown), no seq gaps, two SESSIONs, attempted
   cross-check. The workload includes a **deliberate multi-wake-in-one-ISR**
   case (periodic sleepers whose deadlines coalesce -> one timer ISR wakes
   several threads -> reschedules collapse to a single physical switch) so the
   coalescing path is exercised. The sim flushes the ch1 ring to
   `$KICKOS_TRACE_FILE` at `arch_shutdown`.
4. **QEMU structural run (armv7m tail-asm coverage)** (`check_qemu.py`, ctest
   `telemetry_qemu_structural`, `qemu-telem` preset; SKIP exit 77 without QEMU)
   -- the same `tele_pingpong` + structural checks booted on the `qemu` board
   (mps2-an386), using mps2's semihosting trace clock (section 1); the mps2
   backend writes the ch1 ring to `kicktrace.bin` via semihosting at shutdown.
   This is the ONLY automated coverage of the PendSV-tail C-callback
   (EXC_RETURN save, MSP alignment, `-mgeneral-regs-only`); without it that asm
   is validated only by the manual XMC step.

## 9. Perturbation (stated, measured, partly correctable)

Each emit costs on the order of tens of cycles on armv7m (IrqLock DSB+ISB +
encode + ring copy) and several us on a slow-clocked armv6m part (PRIMASK masks
everything -> this directly inflates the IRQ latency it reports). Mitigations:
(a) the probe overhead is measured at `ktrace_init` (minimum of back-to-back
`arch_trace_now` deltas, clamped to u16) and published in SESSION
`probe_overhead` for host subtraction; (b) the saving grace -- for the **M2 MPU
A/B comparison the constant probe cost cancels in the delta**, so even
uncorrected the baseline-vs-MPU comparison survives (absolute baselines use
(a)). The separate `KICKOS_BENCH` microbench brackets only the save/restore
(before the telemetry emit), so the two instruments do not pollute each other.

## 10. Phasing

- **2a (landed, in-tree):** TCB `uint16_t id` + tid-0 convention;
  `arch_trace_now` on every port (armv7m DWT, mps2/nrf51 semihosting, RP2040
  1 us TIMER, sim us, xtensa CCOUNT, rx CMTW1, riscv rdcycle); the layered
  encoders + record format; `ktrace_*` + SWITCH (physical-swap hook in every
  port's switch path), SYSCALL, IRQ (incl. the timer ISR); the RTT ch1
  record-atomic sink + the CMake publish-barrier seam; `Kernel`-scoped counters;
  `KICKOS_TELEMETRY` + `KICKOS_HAVE_TRACE_CLOCK`; CI gates 1-4; `kicktrace.py`
  (CSV / summary / Chrome-trace / follow / asserts) +
  `tools/telemetry-record.sh`.
- **Manual HW step (open, tracked in `docs/m2-readiness.md`):** capture and
  archive the per-board baselines -- XMC4800 (cycle-accurate, the M2 gate) +
  the coarse operational boards.
- **2b (open):** per-event-class enable flags; SystemView-compatible emission;
  the flight-recorder-on-fault marker (section 12); per-instance sim sinks.

## 11. Resolved decisions

1. `seq` = **u16** per record + **u32 `records_attempted`** absolute anchor in
   SESSION.
2. SESSION = **two bookends**: opening at `ktrace_init`, closing at shutdown
   (`kickos_trace_final_session`). No periodic re-emit: the decoder's two-anchor
   model plus `--clock-hz` cover mid-stream captures, and the clamped one-shot
   timer keeps inter-record gaps far below the u32 wrap.
3. Idle/CPU% **derived** from SWITCH + tid-0 (no hook, no on-target accumulator).
4. IRQ events shipped with 2a (incl. the timer ISR pseudo-line); per-line enable
   is 2b.
5. RP2040 clock: **SysTick stays the kernel one-shot; trace = 1 us TIMER read**;
   the M2-gating cycle-accurate baseline is **XMC/DWT**. (No RP2040 timer
   re-arch.)
6. Loss: seq-gap + `records_attempted`; **a gap poisons open pairs + excludes
   the span**.
7. Emit is **atomic under one `IrqLock`** (seq + stamp + ring write).

## 12. Kernel-aware debugging (the second half of the charter)

The same instrumented stream gives, at increasing cost:
1. **Live scheduling view (shipped).** The SWITCH/SYSCALL/IRQ stream *is* a
   SystemView/Tracealyzer-style timeline (scheduling, ISR nesting, CPU%) without
   halting: `kicktrace.py --follow` for a live record scroll, `--chrome` for the
   timeline. SystemView-compatible emission would borrow SEGGER's viewer (2b).
2. **Flight recorder / post-mortem (2b; high value, low cost -- like NuttX
   `noteram`).** The trace ring lives in target RAM; on a halt/fault a debugger
   reads the last N records -> the exact switch/syscall/IRQ sequence that led to
   the crash, no live host. Only add: a fault-handler marker record, deliberately
   torn-tolerated on a dying system (the one exception to "fault handlers never
   emit", section 3).
3. **On-halt RTOS awareness (follow-on).** A GDB Python OS-awareness script or a
   J-Link RTOS plugin enumerates threads/states/per-thread backtraces on halt by
   reading a well-known thread-list anchor + stable TCB offsets -- reusing the
   same `id`/tid-0 identity model.

## 13. Late-resolved decisions

1. **Flight-recorder marker:** deferred to 2b. Fault/NMI paths emit nothing
   today (section 3); the RAM ring already makes a debugger-read post-mortem
   possible without the marker.
2. **Heartbeat:** none. The MCU kernel one-shot's RVR clamp already forces a
   timer wake (and a traced timer-ISR pair) at least every ~0.12 s @144 MHz /
   ~1.4 s @12 MHz -- far below the DWT ~30 s / TIMER 71 min wraps -- so records
   never go silent long enough for an unseen u32 wrap while anything is
   scheduled. The sim has no periodic wake (`timer_settime` is absolute), which
   is why its trace clock is us (section 1): it wraps at ~71 min, so a CI run
   never aliases.

## 14. Fixed constants + edge cases

- **Reserved `line` ids:** `0xFFFE` = the timer-ISR pseudo-line
  (`TRACE_TIMER_LINE`; distinct from real NVIC lines and from the `0xFFFF`
  no-thread tid).
- **arch id (SESSION.arch, u8):** `0=sim, 1=armv7m, 2=armv6m, 3=xtensa, 4=rx,
  5=riscv (rv32imac)` -- the `ArchId` enum in `include/kickos/trace/record.h`;
  the build bakes it in as `KICKOS_TRACE_ARCH` and the decoder labels the trace
  with it.
- **ch1 ring size:** `KICKOS_RTT_CH1_SIZE`, default 4096 bytes (per-board/preset
  overridable; `sim-telem` uses 16384 because the host drains only at shutdown).
- **No-return syscalls** (`KOS_SYS_EXIT`): recorded as `SYSCALL_ENTER` with no
  `SYSCALL_EXIT`. The decoder tolerates unmatched ENTERs (exit(), or threads
  blocked mid-syscall at shutdown) and flags only orphan EXITs; on-CPU
  attribution ends at the next SWITCH away from the tid.
- **SESSION `ver`:** 1 (`TRACE_VERSION`); the `probe_overhead` field (section 9)
  is part of v1.
