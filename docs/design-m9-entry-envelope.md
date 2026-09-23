<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The M9 entry envelope, recomputed against M8.12

> **Status: ACTIVE.** M9.0's third deliverable, recorded 2026-09-22. It reads frozen numbers
> and writes no code. It is the gate M9.1 through M9.7 were held behind; it approves none of
> them itself, and what it states is what the evidence supports.
>
> **SUPERSEDED IN ONE RESPECT BY M9.1, 2026-09-23, AND THIS DOCUMENT STILL GATES THE STAGES
> BELOW IT.** Every sentence here saying there is no multi-core silicon on this bench is true of
> M8.12 and of the fleet as it stood, and false of the tree today: `esp32-wroom-benchsmp` runs a
> shared kernel on two LX6 cores and is the only multi-core configuration this project has with a
> live cycle counter. What that closes is named in
> [`design-m9-lock-bound.md`](design-m9-lock-bound.md): the locked fraction, absent here on every
> multi-core configuration, forms there. What it does NOT close is the rest -- no silicon
> `lock-wait` above two cores, no cross-core `e2e` on silicon, and no round-trip distribution
> anywhere. A reader planning from this document alone would plan as though that run did not
> happen.

**Why it is recomputed rather than carried.** The scenario arithmetic that sizes M9 rested on
a DERIVED locked fraction, and `roadmap.md` records why that is dangerous: `MPU_APPLY` was
quoted fleet-wide at 443 cycles a switch, 886 of a 3651-cycle locked round trip, and section
8.5 of [`design-m5-ipc-fastpath.md`](design-m5-ipc-fastpath.md) measures the same board at 94
once the PMP precompute landed -- which left every derived multiplier resting on a term a
quarter of its assumed size. So every value below names the input it came from, and where
M8.12 does not carry an input the input is named absent. **Absence is a first-class result
here**: nothing is estimated, and no older figure is put in a missing one's place.

## What this recompute is

Every figure below is read out of `docs/archive/M8.12_meas.md`, at commit `b356b18d`, and out of nothing
else. `docs/archive/M8.13_e2e_addendum.md` is read and cited where it bears on a term, and it
displaces no cell: its own first line rules that not one cell of the exit measurement is touched by
it, and its whole table is a PAIRED delta taken at host loads of 5.5 to 19 against M8.12's 4.2 to
5.5, so it carries no absolute to displace one with. Nothing is carried forward from the old
envelope. Where M8.12 does not carry a term, the term is named absent and no older figure is put in
its place.

Two labels travel with every cell and are not droppable:

- `xmc4800-relax` builds with the MIN headline and prints `min/avg/max` where every other board
  prints `p50/p99/max` (`M8.12_meas.md`, "The instrument is M8.7's"). Field one of an XMC row is
  that board's MINIMUM. Every XMC cell below says so.
- `p50` and `p99` are bucket LOW EDGES at an eighth of an octave (`docs/reference/bench.md`, "What
  the figures may claim"), so both are floors and two figures compare only to that resolution.

And one label that only the reference page carries. `bench.md` rules that a `p99` is stated only
where the row holds 1000 samples, and that the rule was made AT M8.12 -- so the frozen captures
still print `p50/p99/max` on the swept rows. At `n = 50` the 99th nearest rank IS the largest
sample. **Every `p99` on an `e2e`, `irq` or `wcase-irq` row below is the top sample's bucket floor
wearing a percentile's name.** M8.7's record already forbids differencing any swept row's `p99`
against an M8.12 row, and that forbids it in the other direction too.

## The four entry metrics at M8.12, per board and preset

Source for every silicon cell: the named capture embedded in `M8.12_meas.md` section "The captures,
whole". Source for every emulator cell: the `M8.12` column of the table in section "Emulator half:
five runs a preset, six presets", whose cells M8.7's record defines as the MEDIAN of five runs' last
report window.

| board or preset | 1. locked fraction | 2. lock-wait | 3. IRQ-to-user, p50/p99/max | 4. IPC round trip, p50/p99/max |
| --- | --- | --- | --- | --- |
| `f411disco` (armv7m, 84 MHz, enforce) | **0.380**, recomputed below from `lock-hold` 576 cyc and `throughput` 36091 ns/sw, `m812a`/`m812b` | ABSENT -- one kernel core, `BD_LOCK_WAIT` declared above one core only (`bench.md`) | `e2e-local` 26624 / 28672 / 29167 ns, `n=50`, `m812a`/`m812b`; the middle field is the top sample | ABSENT -- no distribution row exists; the only figure is the MEAN 80434 ns at 8 B and 109103 ns at 256 B, `m812a` `call/reply` rows |
| `esp32c6-wroom` (rv32imac, 160 MHz, enforce) | **0.342**, from `lock-hold` 512 cyc and `throughput` 18690 ns/sw, `m812a`/`m812b` | ABSENT, same reason | `e2e-local` 14336 / 45056 / 47581 ns, `n=50`, `m812a`/`m812b`; middle field is the top sample | ABSENT -- MEAN 38594 ns at 8 B, 58157 ns at 256 B |
| `xmc4800-relax` (armv7m, 144 MHz, enforce, **MIN headline**) | ABSENT as a like-for-like -- this board prints no `p50` for `lock-hold`; see "The XMC cannot produce it" | ABSENT, same reason | `e2e-local` **min** 16722 / **avg** 17073 / **max** 18222 ns, `n=50`, `m812d`; wall clock, valid in the dead-counter captures too | ABSENT -- MEAN 50452 ns at 8 B, 68308 ns at 256 B |
| `qemu-riscv-bench` | ABSENT -- the counter rate is 0 Hz | ABSENT -- one core | `e2e-local` 6656, single figure, no `p99`, no `max` | ABSENT -- `rt8` 11374, `rt256` 18089, single figure each |
| `qemu-arm64-bench` | ABSENT -- the counter rate is 0 Hz | ABSENT -- one core | `e2e-local` 8192, single figure | ABSENT -- `rt8` 19475, `rt256` 19473 |
| `qemu-riscv64-bench` | ABSENT -- the counter rate is 0 Hz | ABSENT -- one core | `e2e-local` 6144, single figure | ABSENT -- `rt8` 23652, `rt256` 23459 |
| `qemu-x86_64-bench` | **computable, and alone among the presets**; see "The one emulator preset that declares a rate" | ABSENT -- one core | `e2e-local` 3072, single figure | ABSENT -- `rt8` 5386, `rt256` 5475 |
| `qemu-arm64-benchsmp` (4 cores) | ABSENT -- the counter rate is 0 Hz | **9216**, `lock-wait` row, single figure, no `p99`, no `max`, no contention sweep | `e2e-local` 73728, `e2e-cross` 114688, single figure each | ABSENT -- `rt8` 25410, `rt256` 25662 |
| `qemu-riscv64-benchsmp` (4 harts) | ABSENT -- the counter rate is 0 Hz | **14336**, same shape | `e2e-local` 65536, `e2e-cross` 114688 | ABSENT -- `rt8` 37721, `rt256` 37813 |

Rows deliberately not in that table, and why. `irq` and `wcase-irq` are the IRQ ENTRY span, raise to
the handler's first instruction (`bench.md`, `BD_IRQ_ENTRY`), not IRQ-to-user; they are published
below because M9.1 needs them, but they are not metric 3. The `doorbell` round (13312 arm64, 22528
rv64) is published and is neither of the four. The six rows `M8.12_meas.md` excludes by name --
`REENT_SEAT`, `MPU_COMMIT`, `RECV_LOCKED`, `REPLY_LOCKED`, `REPLY_WAKE`, `REPLY_RECV_TOTAL` -- enter
nothing below as a delta. Where one is stated it is stated as an M8.12 absolute only.

What run would produce each absent cell:

- Silicon `lock-wait`: none. There is no multi-core silicon on this bench (`M8.12_meas.md`, "What
  these figures may NOT be used for"). The run that would produce it is the RK3588-class port, which
  `roadmap.md` places in M11 and explicitly not in M9.
- Emulator locked fraction: **not recoverable by a re-read on five of the six presets, and the
  reason is not a missing column.** The `throughput` line's `ns/sw` is in every capture. What is
  missing is the COUNTER RATE: `qemu-arm64-bench`, `qemu-riscv-bench`, `qemu-riscv64-bench` and both
  `benchsmp` presets all print `cycle counter: 0 Hz`, whose own banner text says no rate converts a
  reading, so `lock-hold` is in cycles, the period is in nanoseconds, and nothing joins them.
  `qemu-x86_64-bench` is the one preset that declares a rate, and its `lock-hold` row consequently
  prints nanoseconds beside cycles, so the fraction forms there and only there. Supplying it on the
  other five is an instrument or configuration change followed by a new capture, not a re-read.
- Emulator `p99` and `max` on every row: the same re-read. The captures carry the full
  `p50/p99/max` triple per row; M8.12's published table carries one field of it.
- IPC round trip as a distribution, anywhere: a new instrument. `bench.md` states outright that on a
  deferred-switch target the round trip is in NO phase row and the throughput row is the only figure
  holding it. There is no `BD_` slot for it. This is an instrument change, not a capture.
- XMC `p50`: not producible on that part. It declares `KICKOS_CHIP_CYCCNT_GLITCHES`, which compiles
  the histograms out entirely (`bench.md`), so the board has no percentile to print. A histogram is
  672 bytes per core per slot against 24 for the accumulator, and that board has the least RAM.

### The IRQ ENTRY rows, published because M9.1 is sized from them

Cycles. Silicon only; the emulator table publishes no `irq` row at all, and M8.7 records why -- the
entry row is void wherever the syscall that raised the line runs masked, which is `armv8a` at every
core count and `rv64imac` at one hart.

| board | `irq` | `wcase-irq[0B]` | `wcase-irq[64B]` | `wcase-irq[256B]` | `wcase-irq[1024B]` |
| --- | --- | --- | --- | --- | --- |
| `f411disco` | 36/36/42 | 48/48/52 | 704/704/763 | 2816/2816/2875 | 11264/11264/11323 |
| `esp32c6-wroom` | 192/192/198 | 208/208/213 | 832/832/854 | 2560/2560/8016 | 10240/15360/15747 |
| `xmc4800-relax` (**min/avg/max**) | 48/48/53 | 57/57/67 | 761/761/761 | 2873/2873/2873 | 11321/11321/11321 |

Every one of these is `n = 100`, so the middle field is the top sample under a percentile's name.

## Metric 1 recomputed: the locked fraction

### The method, taken from the precedent rather than invented

`M8.7_rebaseline_meas.md` section "The locked fraction is now MEASURED, not derived" is the only
place this project has measured the fraction rather than derived it. Its arithmetic, reconstructed
from the terms it names:

    f = (spans per switch) * lock-hold p50 / (cycles per switch)
    cycles per switch = throughput row's ns/sw * the board's counter rate

`spans per switch` is read off the row's own `n`, not assumed: `BD_LOCK_HOLD` samples the OUTERMOST
`IrqLock` bracket, depth zero to depth zero (`bench.md`), and M8.7 reports `n = 80022` against 40000
switches, "two outermost lock spans per switch, plus the 22 the sweep's own setup takes". Applied to
its own capture: `2 * 768 / (39849 ns * 84 MHz = 3347.3 cyc) = 0.459`, which is the "about 46
percent" M8.7 published, and `2 * 3347.3 = 6695` is its "measured 6700 cycles per round". The method
reproduces the precedent exactly, which is what licenses using it on M8.12.

**The same reading IS available at M8.12 on the two `p50` boards and on no emulator preset.** Both
terms are on the silicon captures. Only one of them is on the published emulator table.

### The terms, and the fraction per board

| term | `f411disco` | `esp32c6-wroom` | source |
| --- | --- | --- | --- |
| `lock-hold` p50, cycles | 576 | 512 | `m812a`/`m812b` `lock-hold` row, identical in both captures and all three windows |
| `lock-hold` `n` | 80018 | 80018 | same row |
| switches in the window | 40000 | 40000 | `throughput` row |
| spans per switch, `n` / switches | 2.0005 | 2.0005 | derived from the two above |
| `throughput`, ns/sw | 36091 | 18690 | `throughput` row, median of the capture's three windows (36081/36091/36091 and 18691/18690/18690) |
| counter rate | 84000000 Hz | 160000000 Hz | the capture banner's `cycle counter:` line |
| cycles per switch | 3031.6 | 2990.4 | ns/sw * rate |
| locked cycles per switch | 1152 | 1024 | 2 * `lock-hold` p50 |
| **f** | **0.380** | **0.342** | |

The bucket bound. `p50` is a bucket low edge at an eighth of an octave, so the true median hold sits
in `[576, 576 * 2^(1/8))` and `[512, 512 * 2^(1/8))`. The denominator is an arithmetic mean over
40000 switches and carries no bucket. So from the instrument's resolution alone:

- `f411disco`: **f in [0.380, 0.414)**
- `esp32c6-wroom`: **f in [0.342, 0.373)**

### The one emulator preset that declares a rate

`qemu-x86_64-bench` prints `cycle counter: 1998176554 Hz`, and because a rate exists its
`lock-hold` row prints nanoseconds beside cycles. Both of metric 1's terms are therefore in one
unit on that preset and the fraction forms without a conversion.

**Taken by the extractor's own convention**, which input 12 below establishes rather than assumes
-- field one of the triple, the LAST report window, the median over the five runs: `lock-hold`
p50 in ns 512, 480, 512, 480, 352, median 480; `throughput` 1917, 1997, 1958, 1966, 1942 ns/sw,
median 1958; `n` 80245 over 40000 switches, so 2.006 spans a switch. That is **f = 0.492**.
Pairing the two terms WITHIN each run instead and taking the median of the five ratios gives
0.490, so the two methods agree to a thousandth.

**They agree, and the agreement hides the only thing worth knowing about this cell.** Per run the
fraction is 0.536, 0.482, 0.525, 0.490 and 0.364 -- a spread of 0.172, which is roughly four times
the one-bucket band this document offers elsewhere as its uncertainty. Run 5 lands BELOW
`f411disco`'s 0.380.

| input | f | S(2) | S(4) |
| --- | --- | --- | --- |
| `qemu-x86_64-bench`, column medians | **0.492** | **1.34x** | **1.62x** |
| the same, at the bucket upper edge | 0.536 | 1.30x | 1.53x |
| the run-to-run range, five runs | 0.364 to 0.536 | 1.28x to 1.38x | 1.53x to 1.75x |

**So no ordering against the silicon boards survives its own runs.** The median sits above 0.380
and 0.342 and one run of five does not, on five runs of one preset with no stated tolerance. The
cell is real; a comparison built on it is not, and none is made here.

**And it does not reach the question anyway.** That preset is single-core. Both `benchsmp` presets
read 0 Hz, so metric 1 stays absent on every MULTI-CORE configuration this project has, which is
the reading that matters and is unchanged.

### The XMC cannot produce it, and that is the label and not the board

`xmc4800-relax` prints `lock-hold: 4/954/2472 cyc (min/avg/max, n=80021)` (`m812d`). Field one is a
MINIMUM. A fraction built on it -- `2 * 4 / 3036.2 = 0.003` -- is a true floor and a useless one.
A fraction built on field two, the accumulator MEAN, is `2 * 954 / (21085 ns * 144 MHz = 3036.2) =
0.628`, and that is **a different estimator from the other two boards' and may not be tabled beside
them.** It is stated here because it is the only mean-based reading the fleet has, and because of
what it says in the next paragraph. Its own caveats: this board's DWT was dead in three of four
captures and every cycle cell comes from the single live one, and a `min` of 4 cycles for an
outermost lock span is itself a reading of a part that declares `KICKOS_CHIP_CYCCNT_GLITCHES`.

**The median-based fraction is not the time share, and M8.12 does not carry the time share on either
`p50` board.** A fraction of TIME spent inside the lock is `E[hold] * spans / period`. What is
computed above is `median[hold] * spans / mean[period]` -- a mixed estimator, and the M8.7
precedent's. The mean hold is not printed on a `p50` board; only `p50`, `p99` and `max` are, and a
mean is not recoverable from three order statistics. The one board that prints the mean puts the
same quantity at 0.628 against a median-based 0.380 and 0.342 on its two siblings. That is a hint
about the gap and not a bound on it: different part, different counter, and a counter this record
calls intermittent. **So 0.380 and 0.342 are floors in two independent senses -- the bucket and the
estimator -- and M8.12 carries no upper bound on the time share at all.**

### The workload underneath the fraction is NOT the one the planning documents quote

This is the largest single finding of the recompute and it is not a number.

The fraction above is measured over the **ping-pong throughput workload**: the `throughput` row's
40000 switches and the `lock-hold` row fed by them. The figure `docs/design-m7-smp.md` (around line
207) and `docs/design-multicore.md` section 1.4 plan against -- `f = 0.53`, floor 0.43 -- is
`design-m5-ipc-fastpath.md` section 3.0.4's, and it is over the **call and reply round trip**, a
different and much more kernel-dense workload. `design-multicore.md` section 1.4 states the reason
this matters in its own words: "**`f` is a property of the workload.**"

So the two are not versions of one number. M8.7 said its measurement "supersedes in kind" the 43-to-53
bracket, and in kind is the whole of the claim: same quantity, better method, different workload.

**The call/reply locked fraction cannot be recomputed from M8.12 AS 3.0.4 COMPUTED IT, and only
one of the three reasons first given for that survives checking.** The other two were claims about
a RECORD dressed as claims about the tree, which is the same error input 1 made:

- **`k`, the bracket count per locked leg, is unpublished but it is DERIVABLE, two ways.** The
  correction rule is `composite - k * (NEST - NULL)`, and every frozen capture prints that formula
  in its own phase-table header alongside an `n=` on all 43 rows -- so a mean `k` per composite is
  `sum of the nested rows' n / the composite's n`, straight off bytes already on the box. It is
  also countable in the source, the brackets being lexically visible inlined macros. 3.0.4's own
  values -- 19 for `CALL_LOCKED`, 9 for `RECV_LOCKED`, 6 for `REPLY_LOCKED`, 42 for the trip -- are
  read off tree `22379320` and may NOT be carried across M8.9's fusion of the reply and receive
  paths, which is why they are not reused here. But "unpublished" is the true claim; "no composite
  can be corrected" was not.
- **Which leaves sit inside `IrqLock` is unstated in M8.12 and SETTLED by the code**, and the two
  phases in question land on opposite sides. `PH_REENT_SEAT` is marked inside `switch_book`, whose
  caller holds the lock, so it is INSIDE. `kickos_arch_mpu_commit` is called from the switch
  epilogue in assembly, after `kickos_bench_switch_done` and under its own bare mask, so it is
  OUTSIDE the bracket entirely. Reading membership off a measurement record would indeed have been
  invention; reading it off the code is not, and the code answers.
- **The populations do not match across the three legs.** At M8.12 on `f411disco`, `CALL_LOCKED`
  reads `n = 219999`, `RECV_LOCKED` `n = 259999` and `REPLY_LOCKED` `n = 260000`. 3.0.4's
  arithmetic rests on matched `n` confirming one bracket set per round trip, and says so. These do
  not match, and the phase table aggregates the whole run -- every payload size, both arms, the
  donation step -- while the 8 B round trip is one sweep of 20000 calls.
  **This is the one that stands, and it is a DECOMPOSITION problem rather than a missing
  measurement.** Every term needed exists; what is absent is a way to cut the aggregated phase
  table down to the slice the round-trip figure belongs to. Whether that cut can be made from the
  frozen bytes is not settled here, and settling it is arithmetic to be done rather than a capture
  to be taken.

What CAN be stated, and it is weak. The three bracketed locked legs, summed RAW and uncorrected,
against the 8 B round trip converted at the board's rate:

| board | `CALL_LOCKED` + `RECV_LOCKED` + `REPLY_LOCKED` | 8 B round trip, cycles | raw ratio |
| --- | --- | --- | --- |
| `f411disco` | 2632 + 1299 + 1004 = 4935 | 80434 ns * 84 MHz = 6757 | 0.730 |
| `esp32c6-wroom` | 2650 + 1219 + 966 = 4835 | 38594 ns * 160 MHz = 6175 | 0.783 |
| `xmc4800-relax` | 2938 + 1514 + 1099 = 5551 | 50452 ns * 144 MHz = 7265 | 0.764 |

**That column is not a bound and may not be planned against.** Every term in it is inflated by
`k * (NEST - NULL)` with `k` unpublished; the denominator is an instrumented round trip, not the
uninstrumented one 3.0.4 went to two captures' trouble to estimate; and two of the three numerator
rows are rows M8.12 excludes from like-for-like by name. It is tabled only to show that the
uncorrected reading is far above the ping-pong fraction, which is the direction the workload
argument predicts.

## Metric 2: lock-wait under contention

M8.12 carries this metric on two of nine configurations and nowhere else.

| preset | `lock-wait` | `lock-hold` beside it | source |
| --- | --- | --- | --- |
| `qemu-arm64-benchsmp` | 9216 | 18432 | `M8.12_meas.md` emulator table |
| `qemu-riscv64-benchsmp` | 14336 | 36864 | same |

What it is not:

- **The published table carries one field; the CAPTURES carry the full triple**, checked rather
  than assumed: every window of every run prints `lock-wait` as `p50/p99/max` with a per-core
  breakout beside it, the figures varying between windows. So the distribution is one extractor
  column away and is not a missing measurement.
- **There is no contention sweep, and this is the real absence.** `campaign.sh` varies the preset
  and the repetition and nothing else; both `benchsmp` presets are fixed at four cores. The row is
  whatever contention the bench's own four-core workload produced. The roadmap's phrase "lock-wait
  cycles under contention" implies a level; M8.12 carries no level, no curve against core count and
  no second workload.
- **No silicon, at any core count.** The bench has no multi-core silicon.
- **`wait` and `hold` may not simply be added, and the reason is now settled rather than open.**
  The WAIT SPAN itself lies inside the hold sample, not merely its bookkeeping: `IrqLock`'s
  constructor masks, opens the bench bracket, and only then calls `klock_enter`, whose spin is the
  wait; the destructor leaves the lock, closes the bracket, and only then unmasks. The header says
  so in as many words -- the bracket is inside the mask and outside the kernel lock, so above one
  core the sample holds the wait for the cross-core lock and the lock itself. So `hold` already
  CONTAINS `wait`, `wait / (wait + hold)` is meaningless, and the quantity to derive is
  `(hold - wait)` for the critical section proper.
- **No four-core figure is a kernel cost.** `M8.12_meas.md` rules it, and STATE.md repeats it: there
  is no multi-core silicon on this bench, so nothing separates the kernel's share from the
  emulator's.

The run that would produce the metric as the roadmap words it: a contention sweep at one, two and
four kernel cores on both SMP presets, reading the full triple, with the offered load a controlled
variable. M9.1 is the first stage that takes a new cross-core capture, per the roadmap's own
handover list, so the metric that gates M9.4 does not exist until the stage before it runs.

## Metric 3: IRQ-to-user, the end-to-end span

The span is `BD_IRQ_E2E_LOCAL` and `BD_IRQ_E2E_CROSS`: the raise, through delivery, dispatch, the
tier-1 ISR, the wake, the reschedule, the switch, the return to userspace, the woken thread's read
of its granted window and one trap back in (`bench.md`, "The end-to-end span"). It is reported in
nanoseconds and not cycles, because a span that opens on one core and closes on another cannot be
timed with a per-core counter.

| board or preset | local | cross | `n` | source |
| --- | --- | --- | --- | --- |
| `f411disco` | 26624 / 28672 / 29167 ns | ABSENT, one core | 50 | `m812a`, `m812b` |
| `esp32c6-wroom` | 14336 / 45056 / 47581 ns | ABSENT, one core | 50 | `m812a`, `m812b` |
| `xmc4800-relax` | **min** 16722 / **avg** 17073 / **max** 18222 ns | ABSENT, one core | 50 | `m812d`; the wall clock is valid in the dead-counter captures too |
| `qemu-riscv-bench` | 6656 | ABSENT, one core | not published | emulator table |
| `qemu-arm64-bench` | 8192 | ABSENT, one core | not published | emulator table |
| `qemu-riscv64-bench` | 6144 | ABSENT, one core | not published | emulator table |
| `qemu-x86_64-bench` | 3072 | ABSENT, one core | not published | emulator table |
| `qemu-arm64-benchsmp` | 73728 | 114688 | not published | emulator table |
| `qemu-riscv64-benchsmp` | 65536 | 114688 | not published | emulator table |

Three readings that belong with the cells:

**The p99 column is the top sample.** At `n = 50` the 99th nearest rank is the maximum, so the
middle field is the largest sample's bucket floor. `f411disco` reads 28672 against a max of 29167,
which is one bucket below it, and that is the whole of what the number says.

**`esp32c6-wroom`'s tail is the one cell of this metric that wants a run.** Its `p50` fell from
M8.7's 16384 to 14336, one bucket, while its top sample went from 16657 to 47581, a factor of 2.9,
bit-identical across both captures and all six windows. M8.7's record forbids differencing a swept
row's `p99` against an M8.12 row, and the roadmap rules that an observed maximum is a sample and
establishes nothing, so **neither figure is offered as a change**. What is offered is that the
spread on that board is a first-class unknown and that nothing in M8.12 accounts for it. The run
that would settle it is a swept e2e capture on that board at a sample count that supports a
percentile -- `bench.md` puts that at 1000 -- which is an instrument setting, not a new board.

**The cross-to-local ratio has moved and is no longer one figure.** M8.7 published "a cross-core
wake costs about one and two thirds a local one" as the campaign's only direct reading of what a
core crossing is worth. At M8.12 the two boards give 114688/73728 = 1.56 and 114688/65536 = 1.75,
and they no longer agree to the resolution the phrase implies. Every term is one bucket wide, so the
ratio is uncertain by about a bucket in each direction and the two boards' spread is inside that.
**The honest M8.12 reading is that a crossing costs between about 1.5 and 1.8 local wakes on these
two emulator presets, and that the single fleet figure does not survive.**

## Metric 4: the IPC round trip as p50/p99/max

**M8.12 does not carry this metric, on any board or any preset, in any form.** This is not a gap in
a table; there is no such row in the instrument.

What exists is a MEAN. Each `call/reply` line reports one wall-clock figure per payload size, derived
from a count and an elapsed time -- `call/reply: 8 B  80434 ns/round-trip  (12432 round-trips/s over
20000 calls / 1608 ms)` on `m812a-f411disco-bench.log`. There is no distribution behind it.

`bench.md` states why, in the section "`CALL_RESUME` is a leaf, and the round trip is in no row at
all on a deferred-switch target": on a deferred-switch target the pended switch fires inside the
`IrqLock` destructor, after `CALL_TOTAL` has closed and before `CALL_RESUME` is marked, so the round
trip lands in NO phase row and the throughput row is the only figure holding it. Where `arch_switch`
swaps inline, `CALL_WAKE` swallows the server's whole processing along with everything enclosing it.
There is no `BD_` slot for the round trip.

The means, for the record, all from the `call/reply` rows of the named captures:

| board | 8 B | 256 B | 8 B, generic arm | source |
| --- | --- | --- | --- | --- |
| `f411disco` | 80434 ns | 109103 ns | 98425 ns | `m812a` |
| `esp32c6-wroom` | 38594 ns | 58157 ns | 49507 ns | `m812a` |
| `xmc4800-relax` | 50452 ns | 68308 ns | 61607 ns | `m812d` |
| `qemu-riscv-bench` | 11374 | 18089 | not published | emulator table |
| `qemu-arm64-bench` | 19475 | 19473 | not published | emulator table |
| `qemu-riscv64-bench` | 23652 | 23459 | not published | emulator table |
| `qemu-x86_64-bench` | 5386 | 5475 | not published | emulator table |
| `qemu-arm64-benchsmp` | 25410 | 25662 | not published | emulator table |
| `qemu-riscv64-benchsmp` | 37721 | 37813 | not published | emulator table |

A mean is not a p50 and it is certainly not a p99. The stop condition the roadmap attaches to this
metric -- a partition whose measured overhead exceeds its measured benefit is refused -- is a
statement about tails on a single-flow call, and **M8.12 gives the tail of the IPC round trip
nowhere.** The run that would produce it is a new distribution slot fed at the syscall boundary,
which is instrument work and belongs to whichever stage takes the first new capture.

## The Amdahl figure, re-derived

The formula is `docs/design-multicore.md` section 1.4's, unchanged:

    S(n) = 1 / (f + (1 - f) / n)

Applied to the fraction recomputed above, at both ends of its bucket bound:

| input | f | S(2) | S(4) |
| --- | --- | --- | --- |
| `f411disco`, `lock-hold` p50 576, period 3031.6 cyc | 0.380 | **1.45x** | **1.87x** |
| `f411disco`, bucket upper edge | 0.414 | 1.41x | 1.78x |
| `esp32c6-wroom`, `lock-hold` p50 512, period 2990.4 cyc | 0.342 | **1.49x** | **1.97x** |
| `esp32c6-wroom`, bucket upper edge | 0.373 | 1.46x | 1.89x |
| `xmc4800-relax`, MEAN hold 954, period 3036.2 cyc -- different estimator | 0.628 | 1.23x | 1.39x |

For comparison and not for substitution, the figures the planning documents hold today: 3.0.4's
`f = 0.53` gives 1.31x and 1.55x, and its floor `f = 0.43` gives 1.40x and 1.75x.

### What these figures may not be used for

**They are a different workload from the ones they will be read next to.** 0.380 and 0.342 are the
ping-pong throughput workload's fraction. 0.53 and 0.43 are the call/reply round trip's. The lower
fraction and the higher speedup here do NOT mean the shared-kernel case improved; they mean a
different and less kernel-dense workload was measured. `design-multicore.md` section 1.4 already
rules that a compute-bound pair has `f` near zero and scales nearly linearly, and that this system's
characteristic traffic is close to the worst case. Reading 1.87x off this table as the four-core
payoff for IPC-mediated driver calls is exactly the substitution the section forbids.

**They are floors on the fraction, so ceilings on the speedup.** Both the bucket and the
median-for-mean estimator bias the fraction downward, and a lower `f` gives a higher `S(n)`. The
XMC's mean-based 0.628 sits far on the other side. Nothing in M8.12 bounds the fraction from above,
so nothing in this table bounds the speedup from below.

**No observed maximum enters them and none may.** `roadmap.md`: an observed maximum is a sample and
establishes nothing; the distribution validates an analytical bound and does not replace it. Every
figure here comes from a `p50` or a mean; no `max` row of any capture is a term.

**They are one-core silicon measurements of a quantity about many cores.** They price serialisation
against a workload that never contended, on three single-core parts. They contain no lock-wait, no
cache-line bouncing, no handoff cost and no atomic traffic, all of which are subtractions from the
figure and none of which M8.12 measures on silicon at all. 3.0.4 stated the same caveat about its
own number and it has not weakened.

**No `S(8)` is derived here.** `roadmap.md` rules that eight cores is a scenario and not a target:
no preset configures more than four, no shared kernel has ever run on silicon, and an emulator's
timing model cannot witness cost at any width. An eight-core run answers questions about ring
storage, drain work and arrays that assumed four, never about latency and never the choice between
the two outcomes.

**And no speedup figure here is a freeze.** `design-multicore.md` section 1.4 rules that both bounds
are re-derived after the lock-hold shortening rather than frozen. M8.8 through M8.11 WERE that
shortening, this is that re-derivation, and the next one is owed after M9's own lock work.

## Every input this recompute needed, and what checking the list found

Fourteen were claimed absent. **All fourteen were then checked against the frozen captures and
against the tree: six confirmed, four refuted, three partly, and one right for the wrong reason.**
So five did not survive AS WRITTEN -- the four refuted plus the one whose reason was wrong -- and
three more are true only of part of what they claim. The list below carries a verdict per item,
because a gating document whose absences are unverified is the same class of artifact as the
derived fraction this recompute exists to replace.

**The pattern in every one that failed is the same**: a claim about what a RECORD prints, written
as a claim about what the project can KNOW. A figure absent from a published table is not absent
from the capture it was extracted from, and a fact unstated in a measurement record is not
underivable from the source it measures.

| # | input | verdict |
| --- | --- | --- |
| 1 | the emulator period | wrong reason: it is the counter RATE, 0 Hz on five of six presets |
| 2 | `p99` and `max` per emulator cell | PARTLY: present in the captures; `rt8`/`rt256` are means, not distribution rows |
| 3 | a round-trip DISTRIBUTION | CONFIRMED absent; wrong source cited |
| 4 | a `p50` for `lock-hold` on the XMC | CONFIRMED, and not producible on that part |
| 5 | the accumulator MEAN of `lock-hold` | CONFIRMED |
| 6 | `k`, the bracket count per locked leg | REFUTED: unpublished, but derivable two ways |
| 7 | inside-the-lock leaf membership | REFUTED: the code settles it, and the two phases differ |
| 8 | the UNINSTRUMENTED round trip | CONFIRMED |
| 9 | any `lock-wait` on silicon | CONFIRMED, in the strong form: absent, not `n=0` |
| 10 | `lock-wait` distribution and contention sweep | PARTLY: the distribution is in the captures; only the sweep is absent |
| 11 | whether the wait span sits inside the hold span | REFUTED: it does, and the source says so |
| 12 | the unit and statistic of the emulator cells | REFUTED: the extractor settles all three |
| 13 | a cross-core `e2e` population on silicon | CONFIRMED |
| 14 | any figure on an RX board | PARTLY: true at the frozen tree, false on the current one |

The five that changed, each in one line:

6. **`k` is unpublished, not underivable.** Every capture prints the correction formula in its own
   phase-table header and an `n=` on all 43 rows, so a mean `k` is `sum of nested n / composite n`
   off bytes already on the box; the brackets are also countable in the source.
7. **The code settles membership and the two phases land on opposite sides.** `PH_REENT_SEAT` is
   inside, marked in `switch_book` whose caller holds the lock; `kickos_arch_mpu_commit` is outside,
   called from the switch epilogue in assembly after the bench bracket has closed.
10. **The distribution is there.** Every window of every run prints `lock-wait` as a `p50/p99/max`
    triple with a per-core breakout beside it; the figures themselves vary window to window, so no
    single line stands for the set. `campaign.sh` sweeps preset and repetition and nothing else, so
    the CONTENTION sweep is the true absence.
11. **The wait span is inside the hold sample, not merely its bookkeeping.** `IrqLock` masks, opens
    the bracket, then spins in `klock_enter`; the destructor leaves the lock, closes the bracket,
    then unmasks. So `hold` contains `wait`, and the critical section proper is `hold - wait`.
12. **The extractor settles it.** Field one of the triple, the LAST report window, the median over
    the five runs, in whatever unit the row itself prints -- which makes the published table MIXED:
    cycles for `switch`, `lock-hold`, `lock-wait` and `doorbell`, nanoseconds for `e2e-local` and
    `e2e-cross`, and `rt8`/`rt256` a MEAN in nanoseconds rather than a `p50` at all. The decisive
    check is that the published `qemu-x86_64-bench` `lock-hold` of 960 is the median of the CYCLES
    field; the nanoseconds field's median is 480. M8.13's blanket "`p50`, in nanoseconds" of an
    identically shaped table is wrong in both halves.

**And two of the confirmed ones cite the wrong source**, which is worth carrying because it is the
same failure in a milder form. Input 3's claim is right and the `bench.md` heading it rests on did
not exist at the frozen tree, having landed after it. Input 14 is right at `b356b18d` and wrong
today: an RX bench preset exists on the current tree, and at both trees the gap was configuration
rather than a missing backend, the RXv3 bench wiring being complete at the freeze.

**What still stands, and it is the load-bearing half.** There is no round-trip distribution in the
instrument on any target (3). There is no multi-core silicon, so no silicon `lock-wait` and no
silicon cross-core `e2e` (9, 13). There is no contention sweep at any core count (10). The XMC
cannot print a percentile (4) and no `p50` board prints a mean (5). Those are measurements nobody
has taken, and no re-read produces one.

## What changed against the old envelope, term by term

### `MPU_APPLY` 443 -> 94, and what it takes with it

The correction is real and lands as the roadmap describes. Section 3.0.4's phase table priced
`MPU_APPLY` at 443 cycles a switch on `esp32c6-wroom` because that capture predates the apply/commit
split: the 443 was the pre-split phase with the commit work inside it, and the name did not say so.
Section 8.5 measures the same board after the PMP precompute at 19 plus 75, 94 a switch, 188 a round
trip. **That 94 is a max-based pair and the 93 tabled below is the `p50` pair on the same board**,
which is a one-cycle difference and is marked because this section exists to say that a figure
quoted without its statistic is how the 443 happened.

What rested on the 443 and does not survive:

- **`f = 0.457` and 1.37x**, 3.0.4's own removal arithmetic, which took 886 cycles of `MPU_APPLY`
  out of a 3651-cycle locked round trip. Superseded by 3.0.4 itself and by `design-m7-smp.md`, and
  no replacement is derived here either -- the removal arithmetic needs the corrected locked legs,
  and item 6 above says those are unpublished, so the arithmetic is owed rather than blocked.
- **The sizing of the MPU-apply skip on MMU boards** (`TODO.md`, item P2), which was taken from the
  stale 886 and is recorded as closed on that ground.

What did NOT rest on it, and this is the part worth stating plainly: **3.0.4's 1.31x and 1.40x do
not contain the term at all.** They are the direct and floor readings of the locked legs, and the
443 enters neither. `design-m7-smp.md` says so in its own bullet. So the headline planning figure
survived the repair, and what died was the counterfactual built on top of it.

At M8.12 the same pair on `esp32c6-wroom` reads `MPU_APPLY` 18 and `MPU_COMMIT` 75, raw, against
M8.7's 18 and 75. **The term is stable across the whole exit campaign and needs no further
adjustment.**

### A second term of exactly the same kind, so 443 was not the last one

The same defect -- a phase name that does not cover the work, and a per-board measurement carried
under a fleet-wide sentence -- had a second instance, in the figure that repaired the first.

**94 is a PMP measurement and the armv7m region boards are over twice it.** M8.12 measures the
three protection-programming boards:

| board | `MPU_APPLY` | `MPU_COMMIT` | per switch | source |
| --- | --- | --- | --- | --- |
| `esp32c6-wroom` | 18 | 75 | **93** | `m812a-esp32c6-wroom-bench.log` phase table |
| `f411disco` | 21 | 189 | **210** | `m812a-f411disco-bench.log` phase table |
| `xmc4800-relax` | 27 | 199 | **226** | `m812d-xmc4800-relax-bench.log` phase table |

So the per-switch protection cost is 93 on the PMP board and 210 and 226 on the two armv7m MPU
boards, 2.26 and 2.43 times it. **A figure of this kind is only ever true of the board it was taken
on**, and the repair for the 443 reintroduced exactly the generalisation the 443 had been repaired
for.

**And the same rows moved the same way on the ARM boards, at M8.9, in the same direction as the
original defect.** At M8.7, `f411disco` reported `MPU_APPLY` 23 with `MPU_COMMIT` at `n = 0`, and
`xmc4800-relax` 39 with `MPU_COMMIT` at `n = 0` -- the commit had no ARM feed site, so half the cost
had no bracket on it, which is word for word what section 7 says the 443 was. At M8.12 the same
boards read 210 and 226. **The apparent per-switch protection cost on armv7m rose 9.1x on
`f411disco` and 5.8x on `xmc4800-relax`, and not one cycle of it is a regression.**
`M8.12_meas.md` handles this correctly -- `MPU_COMMIT` is one of the six rows it excludes from
like-for-like by name, and its "What these figures may NOT be used for" section rules the ARM
per-switch TOTAL not comparable with anything published before M8.9. The exit measurement caught it
because it compares rows; a prose sentence quoting one figure cannot.

### A third term, attributed after the freeze, and it sits in this envelope's own table

`esp32c6-wroom`'s `lock-hold` MAX is not a kernel critical section. M8.12 publishes it as 2070
cycles. `TODO.md` around line 3517 records the attribution, which landed at M8.13, after the freeze:
the site is `console_emit` at the instruction after `jal arch_console_write`, the lock held is
`console_tx_insert_line`'s, and the row is a statement about how many bytes the report printed. Ten
extra characters on the window's longest line move it by 220 cycles, at 22.0 cycles an emitted byte,
with no instrument in the image at all.

Two consequences for this envelope:

- **`lock-hold` max may not be used as the longest kernel critical section on that board**, which is
  exactly what M9.1's worst-wait derivation would reach for.
- **A relink on that board is worth 0 or about 170 cycles depending on one address bit.**
  `console_tx_insert_line`'s entry address mod 4 decides it: 2013 to 2016 at phase 0, 2183 to 2186 at
  phase 2, over the captures and builds M8.13 took. **Those are M8.13's trees and the 2070 published
  above is M8.12's**, so the range does not contain it and is not expected to; an absolute on this
  row is a property of the image that produced it. STATE.md's M8.13
  section strikes the earlier claim that a relink there is worth zero. The metric-1 numerator on that
  board is `lock-hold` `p50`, not `max`, so **f = 0.342 is not moved by this** -- but it is a
  two-state term on the same row, and no M8.12 capture records which state its build landed on.

### A fourth term, on the emulators, and it is where M9.1 would look

**`lock-hold` max on both `benchsmp` presets is spin, not a critical section, and its site is
unattributed.** On `qemu-arm64-benchsmp` the row's maximum sits between 2.83 and 4.05 million
cycles in fourteen of fifteen windows, on `qemu-riscv64-benchsmp` between 1.8 and 6.5 million, and
all four cores reach that magnitude in the same window. It is about 163 times the `p50` and 52
times the `p99`; the single-core presets top out near 100 thousand. In 26 of the 30 windows the
`lock-wait` max is 97 to 100 percent of the hold max, and input 11 above establishes why that
composes: above one core the hold bracket CONTAINS the wait, so what the row is reporting at its
maximum is a core spinning, not a core working.

**Why it spins that long is unexplained, and all three candidates fail against these bytes.** The
`lock-site:` line that would name a holder appears in none of the thirty captures. A saturated
wide span would stay in the row and no saturation column is printed on it. And the 0 Hz counter
means the figure cannot be put against wall clock to test host preemption, which is the obvious
suspect on a contended box.

**This is the row `bench.md` calls the figure M9 asks for by name**, so M9.1's worst-case
interrupt-masked window currently has no attributed site on either SMP preset -- the same shape as
the `esp32c6-wroom` finding above, and found the same way, by asking what the maximum is actually
measuring rather than reading it as a bound.

### Terms of the same family already repaired, listed so the next audit does not re-find them

The bracket-correction rule, understated 56-fold before section 4.10 replaced `PH_NULL` with
`PH_NEST`, inflating every composite by about 57 cycles a nested bracket. The four-core IRQ rows of
M8.7's first pass, retracted outright once the probe showed they were the offset between two cores'
counters and not a wait. Section 3.0.3's 38 percent, withdrawn as an uncorrected composite on
`teensy41`. Section 3.0.1's 31 percent, superseded by 43 and 53 once the `kos_recv` park was
bracketed as a third locked leg.

### One term that has moved since the freeze and is NOT in M8.12

`NEST_LOCK`, the calibration row that prices a removed nested lock. It is absent from every M8.12
capture -- the phase tables read 43 rows and carry `NULL` and `NEST` only -- and `TODO.md` states
"the frozen M8.12 capture predates all of it". `bench.md` describes the CURRENT instrument, in which
the calibration site is enclosed by `CALL_LOCKED`, `CALL_TOTAL`, `CALL_SLOW_LOCKED` and
`CALL_SLOW_TOTAL` and is carried by the throughput row as well. **The throughput row is metric 1's
denominator**, so a fraction recomputed from a capture taken after that landed is not comparable
with the ones above. That is a hazard for the NEXT recompute and not a defect in this one.

## What this envelope licenses

It approves nothing. What follows is what the evidence supports and what it cannot reach.

**Supportable from the recomputed numbers.**

- **M9.1**, the lock's own bound, and the case for it got STRONGER under checking rather than
  weaker. Every board whose `lock-hold` maximum could stand in for a worst critical section turns
  out to be measuring something else: `esp32c6-wroom` is reporting how many bytes its console
  printed, and both `benchsmp` presets are reporting a multi-million-cycle spin with no attributed
  site. Metric 2 exists at exactly one contention level. And `klock.h` takes the lock after the
  interrupt mask, so the wait sits inside every core's interrupt-latency bound with no figure on it.
  The stage that measures and bounds the lock is argued for by every one of those.
- **M9.7**, the write-up and the exit measurement, in the narrow sense that the surviving absences
  are its work list: the six confirmed outright, plus the residue of the three partial ones.

**Reachable, but the numbers speak only to the shape and not to the payoff.**

- **M9.2 and M9.3**, ownership and the per-pair rings. The roadmap already rules these land under
  the lock in every outcome, so they need no payoff figure and the envelope owes them none. What the
  envelope does say is that the hold CONTAINS the wait, per input 11, and that **this does NOT make
  the critical section `hold - wait`.** The two are different populations: `klock_attach`
  re-acquires on resume and records a second wait, while the hold bracket's start survives the
  switch, so one hold sample can enclose more than one wait sample. The captures show it --
  `lock-hold` `n = 201004` against `lock-wait` `n = 241951` in one window, about one extra wait per
  switch, and the same gap in every window. Subtracting one column's median from another column's
  median over a different population is not a quantity. **The critical section proper is not
  derivable from the published rows**, and that is an addition to the absences below rather than a
  figure this document can offer.
- **M9.6**, the console contract. The envelope touches it from an unexpected direction: the
  `esp32c6-wroom` finding says the console already owns a kernel lock row's maximum on one board. No
  metric bears on the stage's decision.

**The numbers cannot speak to these at all.**

- **M9.4 and M9.5**, the two stages the stop condition can refuse. The stop condition compares a
  partition's measured overhead against its measured benefit. The benefit side is the locked
  fraction over the workload that partition would serve, which is the CALL/REPLY fraction, and this
  recompute establishes that M8.12 cannot produce it AS PUBLISHED -- but two of the three reasons
  first given for that were wrong, and only the population mismatch stands. `k` is derivable and the
  leaf membership is settled by the code, so what blocks the benefit side is decomposing an
  aggregated phase table down to the round trip's own slice, which is arithmetic nobody has done
  rather than a measurement nobody has taken. **That is a materially weaker claim than this document
  first made, and it is the one the evidence supports.** The overhead side is untouched by any of
  this: a tail on a single-flow IPC call does not exist as a distribution anywhere in the
  instrument, on any target, so the stop condition still has no input on that side. The ping-pong
  fraction of 0.380, 0.342 and 0.492 is a real measurement and it is the wrong workload for this
  decision.
- **Anything about whether the shared kernel beats the coarse lock on silicon.** There is no
  multi-core silicon on this bench, M8.12 says so, STATE.md says so, and the roadmap already rules
  M9's verdict provisional on emulation by construction with the silicon re-check placed in M11.

**And one thing the envelope removes from the table.** The four-core round-trip collapse, which the
archived table prints as -76 percent and STATE.md rounds upward, is M8.12's headline and STATE.md
calls it the figure M9 is sized against. It is not an input
to any of the four entry metrics. It is a reading of this emulator under this workload, both records
say so, and it may not enter a derivation as a saving.
