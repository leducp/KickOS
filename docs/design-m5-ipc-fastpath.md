<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design -- bounding the IPC critical section

> **Status: MEASURED, not yet designed.** Section 1 is the baseline and is a measurement, so it
> does not get rewritten. Everything after it is open.

`../STATE.md` carries no call/reply round-trip figure and neither does `roadmap.md`. One figure
does exist in the tree, in `design-m4.6.2-usb-cdc.md`, but it was taken to size a D-cache decision
rather than the IPC path: one payload, one board, under a USB CDC service list. This page is the
baseline the fastpath is judged against.

## 1. The baseline

Tree `3c3967e3` (master, PR 25), banner clean on every capture. App `user/apps/common/bench`,
which sweeps `kos_call`/`kos_reply` round-trips at six payload sizes between two spawned
equal-priority peers. Logs under `.session/logs/m5base*`.

| board | ISA | clock | posture | 8 B | 32 B | 256 B |
|---|---|---|---|---|---|---|
| `xmc4800-relax` | armv7m M4 | 144 MHz | enforcing | 39252 ns | 42243 ns | 70243 ns |
| `xmc4800-relax` | armv7m M4 | 144 MHz | ring-only | 33001 ns | 35993 ns | 63993 ns |
| `teensy41` | armv7m M7 | 396 MHz | enforcing | 11959 ns | 12244 ns | 16768 ns |
| `esp32-wroom` | Xtensa LX6 | 240 MHz | no ring | 20075 ns | 21884 ns | -- |

Every sweep is LINEAR in the payload, so each board's round trip separates exactly into a fixed
term and a per-byte term.

| board | fixed | fixed, in CYCLES | per byte | cycles/byte |
|---|---|---|---|---|
| `xmc4800-relax` ring-only | 32001 ns | **4608** | 125.0 ns | 18.0 |
| `esp32-wroom` | 19475 ns | **4674** | 75.0 ns | 18.0 |
| `teensy41` | 11700 ns | **4633** | 20.2 ns | 8.0 |
| `xmc4800-relax` enforcing | 38252 ns | 5508 | 125.0 ns | 18.0 |

**The fixed cost is ~4640 cycles on three different ISAs at three different clocks, within 1.5%.**
That invariance is the load-bearing result: a cost that tracks neither the silicon nor the memory
system is a fixed INSTRUCTION COUNT in the kernel path, and it is therefore the thing a fastpath
can remove.

Four further readings, each of which constrains the design:

- **The MPU is 900 cycles, all of it fixed.** Enforcing minus ring-only on `xmc4800-relax` is
  6251 ns at 8 B and 6250 ns at 256 B, so the region reprogram is per SWITCH and touches the copy
  not at all: 450 cycles per switch, 20 percent of the ring-only fixed cost.
- **The copy is `ep_copy`'s byte loop and nothing else.** 18 cycles per byte on armv7m M4, on
  Xtensa LX6 and (per the masked-span sweep below) inside the kernel's own model of it -- three
  ISAs agreeing to three significant figures, which is what a byte-at-a-time loop with a bounds
  test looks like on any of them. The M7 does it in 8 because it dual-issues and is not reading
  through flash wait states. A word-at-a-time copy is worth roughly 4x HERE and nowhere else.
- **The switch is not the cost.** `esp32-wroom` reports the software switch body directly at
  **145 cycles**, min == avg == max over 39998 samples. Two of those is 290 cycles, 6 percent of
  the fixed term.
- **So at the traffic this kernel actually carries, the copy is noise.** `kos_uart_req` is 12
  bytes: 216 cycles of a ~4850-cycle round trip on `xmc4800-relax`, under 4.5 percent.

`esp32-wroom` also gives the exception-entry figures the ARM boards cannot, since it is the one
board in the fleet with no privilege ring (see section 2):

| metric | cycles at 240 MHz |
|---|---|
| software switch body | 145 |
| IRQ entry, best case | 281 |
| inject to entry, no masked span | 307 |
| inject to entry, 64 B masked span | 1011 |
| inject to entry, 256 B masked span | 3123 |
| inject to entry, 1024 B masked span | 11571 |

The masked-span sweep is the kernel's own model of a copy under `IrqLock` and it slopes at
**11.0 cycles per byte**, against `ep_copy`'s 18: the model uses `volatile` arrays and is the
cheaper loop, so it UNDERSTATES the real critical section. It also bounds hardware exception entry
and exit at 307 cycles, which is 6.6 percent of the fixed term.

### 1.1 What the baseline says about the fastpath

A register-payload call shape removes the copy. On this traffic the copy is 4.5 percent. **So a
copy-avoiding fastpath alone is not worth building**, and the seL4 comparison has to be read more
carefully than "it has no memory copy in its fastpath": seL4's fastpath does not merely skip the
copy, it bypasses the generic kernel entry and exit entirely and returns to user from inside the
exception handler. The saving is the WHOLE PATH, and that is where ~4640 cycles have to come from.

The register-payload shape is still wanted, because it is what makes such a path possible at all
(a fastpath that must copy cannot stay in registers), and it is an ABI ADDITION, so it is free
until the freeze. But it is the enabler, not the win, and a design that ships it alone would be
measurable at about 4 percent.

**What the baseline does NOT yet say is where the 4640 cycles go.** Section 3 is that measurement.

### 1.2 It also sizes the SMP decision

`design-m6-smp.md` states that a shared kernel's payoff is Amdahl-bounded by the fraction of a
round trip spent inside `IrqLock`, and that its "about 2x" figure is unproven. The bound cannot be
computed from the numbers above, because the fixed term is not yet split into locked and unlocked
parts -- that is section 3. What section 1 already settles is that the fraction is NOT dominated
by the copy, which is the part a big lock is usually argued about.

**Section 3.0.4 now carries the number: 53 percent measured, at least 43 percent as a floor,
bounding a two-core big lock at 1.31x and 1.40x respectively.** Section 3.0.1's earlier 31 percent
is superseded rather than wrong; it omitted two locked legs.

## 1.3 `xmc4800-relax` CANNOT measure cycles, and this is settled rather than suspected

Its DWT CYCCNT is not merely unreliable, it is DEAD: every phase, the pre-existing `switch.S`
bracket and the IRQ sampler all read zero delta across 120000 samples, and the IRQ lines report
`1/1/1 cyc`, which is the "delta was zero" sentinel rather than a one-cycle latency. That matches
the chip backend's own note that the counter has been "observed returning DWT_CTRL's value" (a
constant, so every delta is 0), and it matches the archived M3 capture where the same board's
`irq` and `wcase-irq` also read `1/1/1`.

**So this board measures WALL CLOCK and nothing else.** It remains the best board for the payload
sweep, since that is a wall-clock measurement and its 144 MHz is now confirmed directly by the
instrument (`core clock: 144000000 Hz`) rather than derived. Do not spend another pass trying to
get cycles out of it.

Two things the instrumented pass did establish on it:

- **The instrument's own cost is about 1450 cycles for 26 bracket halves**, roughly 30 cycles per
  bracket: the 8 B round trip moved from 39252 ns to 49343 ns with 13 nested brackets. That is why
  `PH_NULL` exists and why every phase must be read net of it.
- **The instrument does not perturb the copy.** The payload slope is 125.0 ns/byte instrumented,
  identical to the uninstrumented baseline, so the per-byte term in section 1 is unaffected.

## 2. The instrument, and the two defects the baseline pass found in it

Both are in `user/apps/common/bench`, both were invisible because nothing routinely runs it.

- **The cycle half of the app is dead on every board with a privilege ring.** Its
  `kickos_bench_*` helpers are KERNEL functions called directly rather than syscalls, so they run
  at the caller's privilege, and root is unprivileged on every board by construction. The first
  call faults: `xmc4800-relax` enforcing takes `CFSR=0x82 ADDR=0x20000038` reading
  `SystemCoreClock`, and the ring-only build takes `CFSR=0x8200 ADDR=0xe000e280` reading the NVIC
  instead. Root is the reporter, so the fault ends the run: throughput, switch cycles and IRQ
  latency have not been reachable on ANY enforcing or ring-only board since root went
  unprivileged. `esp32-wroom` reports them because LX6 has no privilege ring at all, which is why
  section 1 has cycle figures from exactly one board.
- **It never terminated**, so a board whose bootloader is reached through `kickos_terminate` could
  not be re-flashed unattended after a bench run. `bench.sh` passes
  `KICKOS_SHUTDOWN_TO_BOOTLOADER` to `teensy41` and the RP boards for exactly that, and
  `reporter_loop` never returned, so the knob was inert here and every bench capture on those
  boards cost a physical button press or power cycle.
  **FIXED, and WITNESSED ON SILICON**: the app now returns after a bounded number of reports, and
  `pizero2350` re-enumerated as `2e8a:000f` BOOTSEL by itself at the end of an instrumented bench
  run, at a new bus device number, with no hand on the board. That capture produced nothing (its
  console cable was on another board and the capture correctly REFUSED on a 0-byte log), so the
  self-recovery is the only thing it witnesses, which is what makes it worth recording here: the
  run had to reach `kickos_terminate` for the board to come back at all.

`esp32-wroom` additionally shows the monotonic clock returning equal values across a multi-
hundred-millisecond window: two of the six payload steps report `0 ns/round-trip ... / 0 ms`
having completed all 20000 calls, and consecutive throughput reports read
`223214285 ctx-sw/s / 0 ms` then `0 ctx-sw/s / 0 ms` then `113472 ctx-sw/s / 352 ms`. The cycle
figures on that board come from CCOUNT and are unaffected; its WALL-CLOCK figures are suspect and
its 8 to 64 B round-trip numbers are quoted above only because they are internally consistent and
agree with the other two boards on cycles per byte.

**Root cause NOT established, and there are two live candidates. Do not pick one without an
instrument.**

- **`kos_clock_now()` conflates failure with a time.** `user/src/syscall_stubs.cc` reads
  `KOS_SYS_CLOCK_NOW` into a local, carries a comment saying the status "must be checked or the
  caller gets an uninitialized time", and then returns **0** when the status is negative. 0 is a
  valid-looking timestamp and no caller in the tree can tell the two apart. Two failed reads give
  `d_ns == 0`, which is precisely the `0 ns/round-trip ... / 0 ms` shape. This is the same class as
  the discarded `kos_irq_attach` return that turned a correct refusal into a silent deadlock on
  `picopi`, and it is a DEFECT ON ITS OWN MERITS whether or not it explains this symptom. What it
  does NOT explain is the `223214285 ctx-sw/s / 0 ms` report, which needs a small NONZERO interval
  rather than two zeros.
- **The TIMG0 shadow latch may be read too early.** `arch_clock_now` on this chip reads TIMG0 T0
  by writing `T0UPDATE` and then reading the `T0LO`/`T0HI` shadow. `chip_esp32.cc` asserts that
  "on the classic ESP32 T0UPDATE has no ready/self-clearing bit (that is an S2/S3 addition); a
  single write latches synchronously". The counter runs on APB/2 while the write is on APB, so
  that is a cross-clock-domain claim, and it is the shape of high-level hardware assertion that
  most often fails to hold. A shadow read that beats the latch returns the PREVIOUS latched value,
  which is a repeated timestamp and would be intermittent with instruction scheduling.

The cheap discriminator is to read the clock twice back to back and count equal consecutive
values, which the bench syscall makes reachable from an unprivileged app on any board.

**The instrumented pass REPRODUCED it and moved the evidence toward the latch.** The same sweep
that reported two of six payload steps as `0 ns` uninstrumented reported **five of six** with the
brackets in, and both throughput reports read `0 ctx-sw/s`. Adding instrumentation changes only
TIMING, so a failure rate that rises with it is a RACE. That fits a shadow read beating the
`T0UPDATE` latch and returning the previous latched value; it does not fit the `kos_clock_now`
failure path, which would be insensitive to how long the surrounding code takes. Neither is proven,
but the latch is now the stronger candidate and the `kos_clock_now` return-0 defect should be
treated as a separate bug that happens to share a symptom.

## 3. Where the fixed cost goes

### 3.0 MEASURED, by phase

`esp32-wroom`, LX6 at 240 MHz (stated by the instrument, not derived), tree `3c3967e3-dirty`
(the instrument is the dirt), log `.session/logs/m5phasew-esp32-wroom-bench.log`. `PH_NULL` is
**1 cycle**, so no phase below needs a meaningful correction. Minimums, which is the statistic to
read.

**This board's COMPOSITE spans are not usable and are omitted.** `arch_switch` is synchronous from
thread context on LX6 (`arch/xtensa/lx6/arch_xtensa.cc`), so `CALL_TOTAL`, `CALL_LOCKED`,
`CALL_WAKE`, `SWITCH_TO` and the `REPLY_*` composites close only when the thread is next resumed
and read as elapsed-until-resumed. `SWITCH_TO`'s max of 214184441 is that, not a measurement. The
LEAF phases are honest on every arch and are what follows.

| leaf | cycles | what it is |
|---|---|---|
| `CALL_MINT` | **400** | `cap_install_reply` into the PEER's table, plus `write_recv_info` |
| `REPLY_LOOKUP` | 303 | `cap_lookup`, the full stale-resolve, the consume, the donor unpark |
| `CALL_VALIDATE` | 222 | the two `user_*_ok` bound checks, OUTSIDE the lock |
| `REPLY_WAKE` | 196 | |
| `CALL_PARK` | 176 | `park_queueless` + `reply_donor_park` + `park_deadline_arm` |
| `CALL_RESOLVE` | 133 | `cap_resolve_e` |
| `REPLY_VALIDATE` | 121 | |
| `KTIME_REARM` | 104 | per reschedule, so twice per round trip |
| `REPLY_FUNNEL` | 103 | `thread_effective_prio` + `sched::set_prio` |
| `CALL_COPY` / `REPLY_COPY` | 92 at 8 B, 2324 at 256 B | 9.1 cycles/byte, matching section 1's 18 for the pair |
| `CALL_RESUME` | 30 | |
| `CALL_PROBE` | 22 | `cap_can_take_reply` |
| `MPU_APPLY` | 13 | the null path; this board has no MPU |
| `CALL_DONATE` | **n = 0** | see below |

**The four costs this milestone set out to rank, ranked.** At 8 B: the reply MINT is 400 cycles and
is the largest; the COPY is 144 for the pair (218 at `kos_uart_req`'s 12 bytes, 4608 at 256); the
CONTEXT SWITCH is 290, being two measured 145-cycle bodies; and the **D1 DONATION is 0 because it
NEVER FIRES**. That last one is a hole in the baseline rather than a cheap result: both bench peers
run at equal priority, so `c->prio > w->prio` is false on every iteration. **The donation question
cannot be answered from this workload**, and pricing it needs a caller outranking the server.

**The mint is the one to note**, because it is the largest of the four AND it is constant: unlike
the copy it does not shrink with payload, so at real driver traffic it is roughly twice the copy.

**About 56 percent is still outside these leaves.** They sum to ~2100 of the ~4818 cycles a round
trip costs at 8 B. `wcase-irq[0B]` on the same capture is 348 cycles, which bounds hardware
exception entry and exit, so two of those plus the two switch bodies accounts for ~990 of the
remainder and leaves ~1730 for syscall entry and exit and the scheduler. That agrees with 3.1
below, which was derived a completely different way.

### 3.0.2 D1 does not fire under SUSTAINED load, which is narrower than it first read

The bench gained a step whose caller runs one priority level ABOVE its server, which is the only
shape `endpoint_call`'s D1 branch admits. **`CALL_DONATE` still reports `n = 0`.** What fires
instead, on both boards, is the SLOWPATH:

| phase | `teensy41` | `esp32-wroom` |
|---|---|---|
| `CALL_DONATE` (D1, fastpath) | n = 0 | n = 0 |
| `CALL_SLOW_DONATE` (D2, boost on enqueue) | 149 cycles, n = 19999 | 160 cycles, n = 19999 |
| `CALL_SLOW_PARK` | n = 20000 | n = 20000 |

**Every single call took the slowpath**, and the mechanism is not a bench artifact. A caller that
outranks its server is woken by the reply (D4) and re-issues its next call BEFORE the lower-priority
server can loop back to `kos_recv` and park. So no receiver is ever parked, `wq_peek_highest` finds
nothing, and the call goes to `send_waiters` every time. The very thing that makes D1 admissible,
the caller outranking the server, is what prevents the rendezvous the fastpath needs.

**So the rendezvous fastpath serves EQUAL-priority peers, and the slowpath serves the
priority-inverted case, which is the latency-sensitive one.** That is backwards from what is wanted,
and it is the sharpest thing the decomposition found.

Consequences, and they settle an open question:

- **The fastpath should REFUSE donation rather than make it cheap.** Section 4.3 posed that as an
  open choice; it is not one. Donation essentially never arrives on the fastpath, so a refusal costs
  nothing measurable and removes a branch and a `sched::set_prio` from the hot path.
- **A work fastpath aimed at high-priority clients must attack the SLOWPATH shape**, or it will
  miss exactly the traffic it exists for. Optimising only the parked-receiver rendezvous optimises
  the case that already has no inversion to fix.
- **Donation is cheap where it does fire.** D2 is 149 cycles on the M7 and 160 on the LX6, and
  end to end the donating step costs nothing observable: 20265 ns against 20030 ns at 32 B on
  `teensy41`.

D1 is not dead code -- a server parked in recv when a higher-priority client arrives is a real
shape, and it is what a request-response service under bursty load looks like. What is now measured
is that a SUSTAINED high-priority caller cannot produce it.

**This section was headed "D1 IS STRUCTURALLY UNREACHABLE" and that was an overstatement**, which
the paragraph above contradicted on its own page. The measurement covers a saturated ping-pong and
nothing else. Under think-time between calls the lower-priority server DOES reach `kos_recv` and
park, and the next call from the higher-priority client finds the rendezvous and admits D1. So the
reachable set is "bursty request-response", which is not an exotic workload -- it is what a driver
service under real traffic looks like, and it is the traffic the fastpath exists for.

### 3.0.3 The nested decomposition, on the one board where spans are honest

`teensy41`, 396 MHz, PMSAv7 with the D-cache on, enforcing, log
`.session/logs/m5pht-teensy41-bench.log`. armv7m PENDS its switch, so unlike the LX6 the composite
spans do not run until the thread is resumed and the nesting is real:
`CALL_TOTAL` contains `CALL_LOCKED` contains `CALL_WAKE` contains `SWITCH_TO` contains
`MPU_APPLY` and `KTIME_REARM`.

**Read AVERAGES here, not minimums**, because the minimum of a parent is not the sum of the
minimums of its children and this board's DWT is reliable enough that the two are close.

| span | cycles | span | cycles |
|---|---|---|---|
| `CALL_TOTAL` | 2183 | `REPLY_TOTAL` | 1357 |
| `CALL_LOCKED` | 1837 | `REPLY_LOCKED` | 1125 |
| `CALL_VALIDATE` (outside the lock) | 216 | `REPLY_LOOKUP` | 270 |
| `CALL_RESOLVE` | 123 | `REPLY_COPY` | 342 |
| `CALL_PROBE` | 16 | `REPLY_FUNNEL` | 120 |
| `CALL_MINT` | 323 | `REPLY_WAKE` | 223 |
| `CALL_PARK` | 169 | `SWITCH_TO` | 343 |
| `CALL_WAKE` | 571 | `MPU_APPLY` | 63 |
| `CALL_COPY` | 370 | `KTIME_REARM` | 66 |

The nesting reconciles: `CALL_LOCKED`'s children sum to 1572 against 1837, leaving 265 for the lock
itself, the queue peek and pop, `write_recv_info` and the `ipc` repurpose. `REPLY_LOCKED`'s children
sum to 955 against 1125.

**The two locked spans are 2962 cycles of a 7859-cycle instrumented round trip, about 38 percent**,
which sits above the 31 percent floor section 3.0.1 derives from leaves alone, exactly as a floor
should. Both figures are inflated by the instrument and by different amounts, so treat 31 to 38
percent as the band and not either end as exact.

> **The 38 percent is WITHDRAWN and the paragraph above is the record of it, not a live figure.**
> It divides an UNCORRECTED composite by an INSTRUMENTED round trip, and 4.10 shows the correction
> it lacked is worth about 57 cycles for every bracket nested inside it. The live number is 3.0.4,
> measured on `esp32c6-wroom` with both controls in the table. Nothing else in this section is
> affected: the per-span figures are the measurement and stand.

**The instrument is far more expensive on this board than on the LX6**: the round trip goes from
4736 cycles uninstrumented to 7859 instrumented, where `PH_NULL` reads 1. The DWT sits in the PPB
and an M7 access there is not a core-register read, and the brackets are optimisation barriers in a
path the compiler otherwise schedules freely. So teensy figures are for RATIOS and nesting; the LX6
carries the cheaper absolute numbers.

### 3.0.1 The Amdahl number, at last

> **SUPERSEDED by 3.0.4.** The route this section takes -- sum the leaves, refuse the composites
> -- is still the right one, and the 31 percent it produced was honestly labelled a floor. Two
> things it could not see have since been measured: the correction rule it read the table under
> was wrong for composites (4.10), and a call/reply round trip has a THIRD locked leg, the
> server's own `kos_recv` park, which this section never counted. Kept unedited because 31
> percent is the number `design-m6-smp.md` and `STATE.md` planned against.

Summing only the leaves that sit INSIDE `IrqLock` -- `CALL_RESOLVE`, `CALL_PROBE`, `CALL_COPY`,
`CALL_MINT`, `CALL_PARK` on the call side, and `REPLY_LOOKUP`, `REPLY_COPY`, `REPLY_FUNNEL`,
`REPLY_WAKE` on the reply side -- gives 1517 cycles of 4818.

**At least 31 percent of a call/reply round trip is spent under the kernel lock.** It is a FLOOR:
the wake path's ready-queue work is inside the lock and is not separately bracketed, and
`CALL_VALIDATE` and `CALL_RESUME` are correctly outside it. `design-m6-smp.md` states that a shared
kernel's payoff is Amdahl-bounded by exactly this fraction and that its "about 2x" figure is
unproven. A floor of 31 percent bounds the speedup of a two-core big-lock kernel at
1 / (0.31 + 0.69/2), about **1.45x**, before any contention -- which is materially below 2x and is
now a measured input to that decision rather than an assumption.

### 3.0.4 The Amdahl number, re-derived where the composites are honest

`esp32c6-wroom`, rv32imac at 160 MHz, ENFORCING (PMP NAPOT), tree `22379320`, log
`.session/logs/m5nest2-esp32c6-wroom-bench.log`. Minimums, n as tabled. Chosen because
`arch_switch` PENDS on rv32imac, so a span closes before the lock does rather than when the
thread is next resumed, which is what makes the LX6's composites unusable (3.0).

Its cycle source is NOT `rdcycle`: the C6 traps on that instruction, so `chip_esp32c6.cc` points
the instrument at the core-clocked CLINT MTIME low word. That is an MMIO load, so the instrument
is dearer here than on any board measured before it: **`PH_NULL` is 2 cycles and `PH_NEST` is 51**,
against 1 and about 57 on the LX6.

**The instrument now carries both controls, and the correction rule is the one in 4.10.** A leaf is
`leaf - PH_NULL`. A composite holding `k` brackets at any depth is `composite - k * PH_NEST`, and
what that produces is the sum of the children's CORRECTED values plus the parent's own unbracketed
work. Two reconstructions check it on this capture:

- `CALL_MINT` reads 494 over two children. `494 - 2 * 51 = 392` against corrected children
  `283 + 108 = 391`. **One cycle in 392.** The old rule would have said `494 - 2 * 2 = 490`, which
  is 99 cycles of work that is not there.
- `REPLY_TOTAL` reads 1296 over eight brackets. `1296 - 8 * 51 = 888` against corrected
  `REPLY_LOCKED` 738 plus corrected `REPLY_VALIDATE` 132, leaving 18 cycles for the
  `sched::current()` call and the `IrqLock` between them.

#### A round trip has THREE locked legs, not two

`SWITCH_TO` reports `n = 280021` and `PICK_NEXT` reports `n = 400028` over 140000 round trips:
**two switches and three reschedules per round trip**, which is not the shape section 3.0.1
assumed. With equal-priority peers the reply's wake does NOT switch -- `pick_next` returns the
still-running server -- so the two switches are the call's wake and the server's own `kos_recv`
park. That park holds `IrqLock` and nothing had ever bracketed it. It is now `RECV_LOCKED`.

#### The direct number

| locked leg | composite | brackets inside | corrected | its leaves, corrected |
|---|---|---|---|---|
| `CALL_LOCKED` (fastpath arm) | 2790 | 19 | **1821** | 1560 |
| `RECV_LOCKED` (parking arm) | 1551 | 9 | **1092** | 845 |
| `REPLY_LOCKED` | 1044 | 6 | **738** | 552 |
| **per round trip** | | **34** | **3651** | **2957** |

The bracket counts are read off the source and every one of them is confirmed by an `n` column:
`CALL_LOCKED` and `CALL_MINT` both report 120000, `RECV_LOCKED` 120000, `REPLY_LOCKED` 140000,
and the shared wake-path phases report exactly two switches and three reschedules per trip.

The leaves that make up the 2957, each net of `PH_NULL`:

| leg | leaves |
|---|---|
| call | `CALL_RESOLVE` 134, `CALL_PEEK` 16, `CALL_PROBE` 17, `CALL_POP` 51, `CALL_COPY` 94, `CALL_MINT_CAP` 283, `CALL_MINT_INFO` 108, `CALL_PARK` 149, `WAKE_UNPARK` 48, `PICK_NEXT` 59, `SWITCH_BOOK` 70, `MPU_APPLY` 443, `KTIME_REARM` 67, `ARCH_SWITCH` 21 |
| recv | `RECV_RESOLVE` 150, `RECV_SCAN` 35, `PICK_NEXT` 59, `SWITCH_BOOK` 70, `MPU_APPLY` 443, `KTIME_REARM` 67, `ARCH_SWITCH` 21 |
| reply | `REPLY_LOOKUP` 266, `REPLY_COPY` 94, `REPLY_FUNNEL` 85, `WAKE_UNPARK` 48, `PICK_NEXT` 59 |

**The 694-cycle gap between the floor and the direct number is identified, not residual.** It is
26 cycles of unbracketed body in `CALL_LOCKED`, 6 in `REPLY_LOCKED`, 0 in `RECV_LOCKED`, plus 44
inside `SWITCH_TO` and about 617 in the `sched::wake` / `wq_block` plumbing: four nested `IrqLock`
constructions per wake, `resched_after_wake`'s two state tests and `reschedule`'s
`next == current` test. That is lock and call-frame overhead, not hidden algorithmic cost, and it
is why the two ends differ by as much as they do rather than by nothing.

#### The denominator, and why its uncertainty does not matter

The instrumented 8 B round trip is 55555 ns, which at 160 MHz is 8889 cycles. Forty-two brackets
execute per equal-priority round trip (23 call, 10 recv, 9 reply), and a bracket costs the path it
sits in `PH_NEST - PH_NULL` = 49 cycles.

That constant is not taken on faith. **Two captures on this board differ by exactly four brackets**
-- `m5nest` at 38, `m5nest2` at 42 -- and their 8 B round trips differ by 55555 - 54392 = 1163 ns,
which is 186 cycles, or **46.5 cycles per bracket measured differentially**. So the uninstrumented
8 B round trip is 8889 - 42 * 46.5 = 6936 cycles, or 6831 using the 49. Take **6830 to 6940**.

| | locked cycles | of 6936 | of 6831 |
|---|---|---|---|
| leaf floor | 2957 | 42.6% | 43.3% |
| direct, corrected composites | 3651 | 52.6% | 53.5% |

**At least 43 percent of a call/reply round trip is spent under the kernel lock, and the direct
measurement puts it at 53 percent.** The two do not coincide and are not meant to: the floor omits
the 694 cycles of lock plumbing itemised above, all of which is inside the lock. The floor is the
number that survives if the correction constant is wrong by any amount; the direct number is the
one to plan against, and the two reconstructions above are what license it.

A two-core big lock is Amdahl-bounded at `1 / (f + (1 - f) / 2)`:

- `f = 0.526` gives `1 / (0.526 + 0.237)` = **1.31x**
- the floor `f = 0.426` gives `1 / (0.426 + 0.287)` = **1.40x**, an upper bound on the speedup

The denominator's own 1.6 percent spread moves either figure by under 0.01x, so an exact
uninstrumented capture would not change the decision and is not worth a second knob.

**This is an ENFORCING board, and that is not what carries the result.** `MPU_APPLY` is 443 cycles
per switch and both switches are inside the lock, so the PMP reprogram alone is 886 of the 3651.
Removing it entirely gives 2765 locked of 6050, `f = 0.457`, and **1.37x**. So no posture of this
board reaches the "about 2x" `design-m6-smp.md` assumes.

**What this replaces, and by how much.** Section 3.0.1's floor was 31 percent bounding 1.45x. The
new floor is 43 percent bounding 1.40x, and the direct number is 53 percent giving 1.31x. The
floor moved because two whole locked legs were missing from it: the wake path's ready-queue work
and the entire `kos_recv` park. The withdrawn 38 percent from 3.0.3 is not resurrected -- it came
from an uncorrected composite on `teensy41` and the correction it lacked is worth about 57 cycles
per bracket there.

### 3.1 The same answer, reached independently

**From a comparison already inside the baseline capture rather than from the phase decomposition.
Kept because it was derived first, by a different route, and agrees.**

`esp32-wroom` reports the semaphore ping-pong beside the call/reply sweep, from ONE image at ONE
tree. A ping-pong round is two syscalls and two context switches and nothing else: no copy, no
capability resolve, no reply-cap mint, no priority donation, no reply lookup, no funnel recompute.
Its round costs 8812 ns per switch, so 4230 cycles for the two. The call/reply FIXED term on the
same board is 4674 cycles.

| term | cycles | share of the fixed cost |
|---|---|---|
| two syscalls plus two context switches, the generic path | ~4230 | **90 percent** |
| everything IPC-specific except the copy | ~444 | 10 percent |
| of which the software switch BODIES (145 x 2) | 290 | 6 percent |

**All four of the costs this milestone set out to rank are inside that 10 percent**, and the copy
is a further 4.5 percent at real traffic. The 90 percent is generic syscall entry and exit, the
scheduler, and hardware exception entry: machinery `kos_call` shares with `kos_sem_post`.

That also explains the cycle-invariance in section 1, which is otherwise strange given that the
COPY loop differs by 2.25x between the M4 and the M7 in the same captures. An invariant cost is
neither MMIO-bound nor memory-bound: an MMIO-dominated term would scatter, because the same
`arch_clock_now` is a chained four-slice CCU40 read on the xmc, a GPT read on the RT1062 and a
single-cycle core-register read on the LX6. It is the same structural path on every arch.

**Two reservations, and neither is discharged.** This rests on the ONE board whose wall clock
section 2 records as intermittently freezing, so it wants reproducing on the xmc or the teensy.
And the ping-pong runs prio-1 peers where the call/reply sweep runs prio-4 peers, which is not
identical scheduling. The phase decomposition supersedes this section when it lands; until then
read the 90/10 split as the right order of magnitude and not as four significant figures.

Two hypotheses were considered and REJECTED on the numbers before this one, and they are recorded
because each is the obvious guess:

- **`ktime_rearm`**, called on every reschedule and reading the 64-bit clock. Rejected by the
  cycle-invariance above: its clock read is a peripheral access on two of the three parts and a
  core register on the third, so a term dominated by it could not land within 1.5 percent across
  them.
- **`cap_reply_live`'s flat-path scan**, which `design-capability-table.md` section 8 indicts as a
  walk of a PEER's whole table, run on the fastpath by `cap_can_take_reply`. Structurally it is
  the right shape, a loop whose trip count is a configured constant and therefore arch-invariant.
  Rejected on magnitude: it runs `KICKOS_CAP_FIRST_DYNAMIC` to `thread_cap_capacity(c)`, a child
  cap width, so it is tens of iterations at a few cycles each. Two orders of magnitude short.

## 4. The design

OPEN in its mechanism. Two things about its SHAPE are ruled already, and both are constraints on
whatever gets written here.

### 4.1 The caller never chooses the path

**A fastpath a user must opt into is a fastpath nobody takes.** If the register-payload shape is a
separate entry point beside `kos_call`, then every driver author has to know which to call, every
driver written before it never benefits, and the tree carries two APIs for one operation with the
faster one cold. That is the same failure as a service nothing links.

So `kos_call` stays the only thing anyone writes, and **the substrate picks the form**: the stub
branches on the payload lengths and traps through the register-carrying syscall number when the
request and the reply both fit the ABI's argument registers, and through today's buffer-carrying
number otherwise. The ABI addition is INTERNAL, a number the stub selects, not a fork a consumer
sees.

The decision cannot move into the kernel, and that is worth stating because it looks like it
could: the payload has to be IN registers at the trap instruction for the register form to save
anything, so the choice is made before the kernel is entered. The stub's compare is a couple of
instructions against a round trip measured at ~4640 cycles.

This fits the traffic rather than hoping for it: `kos_uart_req` is 12 bytes and `kos_uart_rsp` 8,
three words and two, so the register form is what every driver call in the tree would take without
one line of driver change.

It also gives the wall of refusals two levels, and they must not be confused. The STUB refuses on
size, statically and before trapping. The KERNEL fastpath refuses on state (no parked receiver, a
dying peer, a full table, donation applicable) and falls through to the slowpath. Correctness lives
in the slowpath either way, and neither refusal may be a case the slowpath cannot handle.

### 4.2 What it has to attack is the whole path, not the copy

Section 1.1 and section 3 agree from two directions: the copy is 4.5 percent of real traffic and
everything IPC-specific is 10 percent. A fastpath that removes the copy and re-enters the generic
syscall and scheduler machinery is worth about four percent. seL4's is fast because it never enters
that machinery, returning to user from inside the exception handler. **The register shape is the
ENABLER for that** -- a path that must copy cannot stay in registers -- and not the win itself.

### 4.3 The donation question is ANSWERED: refuse it

This section previously said the choice between making donation cheap and having the fastpath
refuse it could not be settled without a measurement. The measurement exists now and it settles it,
though not the way it was framed. Section 3.0.2 has the evidence.

**Refuse -- but on a NARROWER argument than this section first gave, and see 4.9.** D1 is
admissible only when the caller outranks the server, and a caller that outranks its server never
finds a parked receiver *in sustained traffic*, because it re-calls before the server can loop back
and park. The refusal therefore costs nothing IN THAT WORKLOAD. It does not follow that donation
never reaches the fastpath: under bursty load the server does park, and then the D1 rendezvous is
exactly the shape the fastpath serves. Section 4.9 is what that costs.

**The harder consequence is about WHICH path the fastpath should be.** The rendezvous fastpath
serves equal-priority peers; the slowpath serves the priority-inverted case, which is the one with
latency to save. A work fastpath that only handles the parked-receiver rendezvous optimises the
traffic that has no inversion to fix. Whatever section 4 eventually specifies has to say what it
does for a high-priority client, or it is aimed at the wrong half.

### 4.4 The budget, and what a fastpath can and cannot take

Section 3.0's ~4818 cycles at 8 B on the LX6, sorted into what a fastpath may attack and what it
may not.

| part | cycles | attackable |
|---|---|---|
| hardware exception entry and exit, twice | ~696 | **no**, this is the silicon |
| the two `arch_switch` bodies | 290 | **no**, two threads must actually swap |
| `MPU_APPLY`, twice | 26 here | **no**, and this board understates it |
| IPC leaves (mint, lookup, validate, park, resolve, funnel, copy, probe, resume) | ~1800 | partly |
| `KTIME_REARM`, twice | 208 | partly |
| syscall entry and exit, and the scheduler | ~1730 | **yes, and this is the prize** |

**The floor is about 1000 cycles plus the MPU**, so the whole exercise is bounded at roughly 4.8x on
this board and less on any board with a real MPU. That is the number to hold a design against, and
it is worth having before writing code rather than after.

`MPU_APPLY` reads as 13 cycles only because `esp32-wroom` has no MPU. On every board that has one it
is a real region reprogram, it is per-round-trip twice, and **the fastpath may not skip it**: a
client and a driver service live in different domains by construction, which is the whole point of
the isolation. The one case where it collapses is IPC between threads of the SAME task, which is a
pointer compare and worth taking, but it is not the driver traffic.

### 4.5 The first structural decision is the switch, not the copy

`arch_switch` PENDS on armv7m, rv32imac and rxv3, and swaps INLINE on sim and Xtensa LX6. A fastpath
that returns to user from inside the exception handler, which is where the ~1730 cycles live, cannot
pend: pending means returning to the generic exit path and letting it reschedule, which is precisely
the machinery being bypassed. So the fastpath has to perform the switch itself.

That is a new arch seam, not a tweak to an existing one, and it is the first thing to specify. seL4
answers it by writing the fastpath per-architecture in assembly. KickOS does not have to go that
far, but it does have to answer the same question, and the answer determines whether the fastpath is
one portable C body with a small arch hook or five arch bodies. **Nothing else in section 4 can be
sized until this is decided.**

### 4.6 The mint is the next measurement, not yet a ruling

`CALL_MINT` is 400 cycles, the largest leaf, and constant in the payload, so at real driver traffic
it is roughly twice the copy. It is also **two different things measured as one**:
`cap_install_reply` and `write_recv_info`. `write_recv_info` writes the receiver's `kos_recv_info`
in USER memory; `cap_install_reply` walks the free list and seats a generation. They have nothing in
common and only one of them is a capability operation. **Split the phase before designing around
either number.**

Two facts to carry into that measurement, both read off the tree rather than measured:

- `cap_reply_live` (`kernel/syscall/cap.cc:743`) is an O(capacity) SCAN on the flat path
  (`KCAP_RUN_CHUNKS == 1`) and an O(1) counter on the segmented path, and the call path runs it
  TWICE, once in `cap_can_take_reply` and once inside `cap_install_reply`. `esp32-wroom`'s
  `CALL_PROBE` of 22 cycles says that board is not paying for a scan, so the boards that DO take the
  flat path are the ones to measure. Which board takes which path is a build fact
  (`KICKOS_MAX_HANDLES <= KCAP_CHUNK_TARGET`), and it has been recorded backwards before.
- The reply capability is minted with **rights 0**, and `cap.cc:749` states that this makes it
  undelegable. Its count is separately bounded by `KICKOS_CAP_REPLY_MAX`.

That second fact is the interesting one, and it suggests a direction rather than settling it. **A
capability that cannot be delegated and whose live count is already bounded does not obviously need
a slot in the general table.** seL4 does not put it there; the reply lives in the caller's TCB. If
KickOS reserved `KICKOS_CAP_REPLY_MAX` slots per thread instead of minting into the general run,
the free-list walk goes away, the probe goes away, `-KOS_EMFILE` stops being a reachable outcome of
the reply path, and `CALL_MINT` plus `REPLY_LOOKUP` -- 703 cycles, about 15 percent of a round trip
-- come under attack together.

**It is NOT ruled here**, for two reasons. The split measurement might show most of the 400 is
`write_recv_info`, in which case the slot buys much less than it looks like. And the reply path's
correctness lives in the stale-resolve and the teardown, which are exactly what the generation and
the general table give it for free; a reserved slot has to re-earn both. That is a design question
with a real refusal available, and it deserves its own pass.

### 4.7 What the fastpath does for a high-priority client, which 4.3 left open

Section 4.3 ended by warning that a rendezvous fastpath might be aimed at the wrong half of the
traffic, because the rendezvous is the equal-priority case and the inversion is where the latency
is. That warning is answerable, and the answer is that there is no second fastpath to write.

When the caller outranks the server, section 3.0.2 measured that **no receiver is parked**: the
caller re-issues before the server can loop back and park. So at the moment the high-priority caller
traps, there is nothing to hand off to. Its latency is not path length through the kernel, it is how
long until the server is SCHEDULED, and no amount of shortening the call path changes that. What
changes it is the boost, D2, which already fires and is already measured at 149 cycles on the M7 and
160 on the lx6.

So the two halves are served by two different mechanisms, and both exist: **the fastpath serves the
rendezvous, and D2 serves the inversion.** The fastpath is not aimed at the wrong half; it is aimed
at the only half where path length is the cost. This is the answer 4.3 asked for, and it means the
fastpath's refusal list in 4.1 can keep "no parked receiver" as a plain fall-through with no guilt
attached to it.

### 4.8 The seam, located: KickOS already LEAVES the handler before it dispatches

Section 4.5 said the switch is the first thing to specify and that the answer decides whether the
fastpath is one portable C body or five arch bodies. Reading the armv7m trap path answers it, and
the answer is sharper than "the switch pends".

`arch/arm/armv7m/switch.S` routes a syscall like this: the user's `arch_syscall` issues `SVC`,
`SVC_Handler` runs, and it **exception-returns into `svc_trampoline`, which runs in PRIVILEGED
THREAD MODE on the calling thread's own PSP**. Only then does `bl syscall_dispatch` happen. The
comment states the intent outright: dispatch runs in thread mode so that a blocking syscall is an
ordinary synchronous PendSV switch, with the mid-dispatch continuation frozen on that thread's own
stack and resumed inline when it is next scheduled.

**That is the opposite structure to seL4's fastpath**, which never leaves the handler: it completes
the transfer and exception-returns straight into the receiving thread. KickOS pays a full exception
return, a thread-mode call frame, the generic dispatch, and then a PendSV round trip to switch --
and section 4.4 already measured that region at about 1730 cycles, the largest attackable block in
the round trip.

So the seam is not a new `arch_switch` variant. **It is a decision point inside `SVC_Handler`,
before the exception return.** If the fastpath conditions hold, that code completes the IPC and
rewrites the exception return so it lands in the TARGET thread, never entering `svc_trampoline` and
never calling `syscall_dispatch`. If they do not, it falls through to exactly today's path.

Three consequences follow, and they are what makes this worth writing down now.

- **The frame surgery is inherently per-arch**, because it edits the saved exception frame and the
  stack pointer of the incoming thread. On armv7m that means the `{r4-r11, EXC_RETURN}` block and
  the hardware frame, with the FP conditionality on `EXC_RETURN` bit 4 that `switch.S` already
  handles. That part cannot be portable C, and it is why seL4 writes its fastpath in assembly per
  architecture.
- **The capability work above it CAN be one shared C leaf** -- resolve, probe, mint, copy, the
  refusals. So the split is not "one body or five": it is ONE C leaf plus a small arch prologue
  and epilogue per backend, which is the shape to aim for and is much cheaper than five bodies.
- **The two inline-switch backends (sim, Xtensa LX6) need a different epilogue**, not a smaller
  one. They have no pended switch to redirect, so their fastpath completes the swap inline exactly
  as `arch_switch` does today. That asymmetry is the same one that made the LX6's composite bench
  spans unusable in section 3.0, and it will show up again here.

**What this does NOT change is the refusal list.** Correctness still lives in the slowpath, and the
fastpath must fall through rather than reimplement any refusal -- which is now a stronger constraint
than it sounds, because falling through from inside the handler means unwinding to the normal
exception return with nothing committed. **The fastpath must therefore decide BEFORE it mutates
anything.** That ordering requirement is the first thing to write into the implementation, and it is
the reason the probe (`cap_can_take_reply`, 22 cycles) exists as a separate step from the mint.

### 4.9 Refusing donation is a real concession, and the prior art says why

Sections 3.0.2 and 4.3 concluded "refuse donation" from a saturated ping-pong, where D1 never fires.
Under BURSTY load it does fire, and it fires in precisely the rendezvous the fastpath is built for:
a server parked in `kos_recv` when a higher-priority client arrives. So the refusal is not free in
general -- it drops exactly the high-priority-client case to the slowpath, which is the case with
latency to save. **That is a concession, and this page previously read as though it were not one.**

The reason it is hard here is what donation COSTS in this design, and the contrast with the prior
art is the useful part rather than the ranking.

- **KickOS donates a PRIORITY.** D1 raises the server's effective priority, which goes through
  `sched::set_prio`, which is a ready-queue re-seat plus a bitmap update (`sched.cc` states it is
  the sole writer of an effective priority, for exactly this reason). That is real work on a path
  whose whole purpose is to do almost none, and D2 measures the same family at 149 cycles on the M7
  and 160 on the lx6.
- **seL4 (MCS) donates a SCHEDULING CONTEXT** -- a budget and a period -- and it does so ON its
  fastpath (`../nuttx/seL4/src/fastpath/fastpath.c`, and `schedContext_donate` in
  `src/object/schedcontext.c`). It is cheap there because the receiving thread is required to have
  NO scheduling context of its own, so the transfer is a pointer move rather than a recomputation.
  That is the "passive server" model: the server owns no CPU time at all and runs only on time a
  client lends it.
- **Inherit-on-receive**, where a server adopts the priority of the message it accepts, is the
  classic message-passing answer and is what KickOS's D2 already is. It fires, it is measured, and
  it is not the thing under discussion.

**So the question is not whether donation is feasible -- it is implemented here and it works. It is
which currency is donated.** A priority must be recomputed against a run queue; a budget can be
moved by assignment when the receiver is guaranteed to hold none. If the fastpath is ever to admit
donation, the seL4 shape is the one to study, and the precondition to look for is the equivalent
guarantee: a server that cannot already hold the thing being handed to it, so the handoff is a
store and not a re-seat.

**Not decided here.** Admitting donation to the fastpath is a design change with a real refusal
available, and it interacts with D3's revert-by-recompute, which is the half that would still have
to run somewhere. What IS decided is that "donation never arrives" is not a valid reason, and this
page no longer offers it as one.

### 4.10 The mint, split -- and the instrument was lying about composites

`CALL_MINT`'s 400 cycles are now two numbers. `esp32-wroom`, LX6 at 240 MHz, minimums, n = 120000,
log `.session/logs/m5mintsplit-esp32-wroom-bench.log`:

| leaf | cycles | what it is |
|---|---|---|
| `CALL_MINT_CAP` | **290** | `cap_install_reply` |
| `CALL_MINT_INFO` | **109** | `write_recv_info` |

They sum to 399 against the pre-split 400, which is `PH_NULL` exactly, and twelve other leaves in
the same capture are byte-identical to the section 3.0 pass. So the split is placed honestly: the
two operations are strictly sequential, with only an assert between them.

**Section 4.6's premise was wrong and is withdrawn.** It said the mint "walks the free list". There
is NO walk on this path: `cap_run_peek_free` (`kernel/include/kickos/cap.h:454`) is a head peek that
returns on its first test, and the unlink beside it is an O(1) doubly-linked removal. The 290 is not
a search. It is mint MACHINERY -- a pointer-to-pool-index division out of `index_of`, `handle_for`,
three out-of-line call frames on Xtensa's windowed ABI, the chunk indirection, the generation seat
and the sequence-bitfield read-modify-write.

The one O(capacity) thing in the neighbourhood is `cap_reply_live` on the FLAT path, and **this
board does not compile it**: `KICKOS_MAX_HANDLES` 10 against `KCAP_CHUNK_TARGET` 8 selects
SEGMENTED, so the bound test is the O(1) counter. Confirmed in the emitted code, not inferred from
the knob. `KICKOS_CAP_REPLY_MAX` is 1 here. **A flat-path board still pays that scan twice per call
and is still unmeasured**; nothing on this page speaks for one.

`CALL_MINT_INFO`'s 109 is corroborated sideways: `write_recv_info` reaches user memory through an
8-byte byte loop, and `CALL_COPY` at 8 B is 92 for the same loop one frame shallower. So delivery
costs about what the request copy costs and is irreducible without changing the copy primitive.

**This strengthens the reserved-reply-slot idea rather than weakening it.** The larger half is
exactly what a reserved per-thread slot removes, and it removes 290 of machinery rather than a scan
that was never there. With `KICKOS_CAP_REPLY_MAX == 1` on this board, "reserve `KICKOS_CAP_REPLY_MAX`
slots" is literally one slot. What survives is the 109, which is delivery and not capability work.
Carry one caveat: the 290 is call-chain-dominated on a windowed ABI, so an armv7m part will not
scale from this figure.

#### The instrument defect, which is the bigger finding

`KICKOS_BENCH_SPAN(phase, var)` expands to `bench_phase_add((phase), bench_cyccnt() - (var))`, so
the closing timestamp is evaluated as an ARGUMENT -- **before** `bench_phase_add` runs. `PH_NULL`
therefore measures two counter reads and nothing else, which is why it reads 1 cycle. But a bracket
NESTED inside an enclosing span charges that parent the two reads *plus* the whole accumulator call:
the out-of-line frame and the min/max/64-bit-sum update, about 57 cycles here.

The new parent `CALL_MINT` reads 514 where its two children sum to 399, and 514 - 399 = 115 is two
such charges, not the two cycles `PH_NULL` predicts.

**So `bench.h`'s stated correction rule, that a phase is `(phase - k * PH_NULL)`, understates the
per-bracket charge by roughly 56x, and every COMPOSITE in section 3.0's table is inflated by about
57 cycles per bracket nested inside it. Only the LEAVES are honest.** This page already refused the
LX6's composites for a different reason (its `arch_switch` swaps inline, so they read as
elapsed-until-resumed); this is a second and independent reason, and it applies to boards where the
switch pends too.

**What this puts in doubt, stated rather than quietly dropped:** section 3.0.1's locked fraction is
a band whose 31 percent end is a LEAF floor and survives, and whose 38 percent end came from a
composite on `teensy41` and does not. The band's upper end is therefore unproven and the honest
reading today is "at least 31 percent". Since that number is what sizes the M6 big-lock Amdahl
bound, **it should be re-derived from leaves before anyone plans against it**, and section 3.0.3's
"31 to 38 percent" should be read as "31 percent measured, upper end withdrawn" until then.

#### FIXED, and the fix found a second defect of the same family

`PH_NEST` is `PH_NULL` with an enclosing span wrapped around it, so it prices exactly what a
complete nested bracket costs its parent: the inner mark, the inner closing read AND the inner
accumulator call. The correction rule in `bench.h` and in the printed table header is now
`leaf - PH_NULL` and `composite - k * PH_NEST`, and 3.0.4 checks it two ways on silicon.

The rule leaves `(k - 1) * PH_NULL` on the table for a parent with `k` children in series, because
one counter read serves as the close of one bracket and the open of the next. On `esp32c6-wroom`
that is 2 cycles against a 392-cycle `CALL_MINT`, and it is the correction's own floor.

**The second defect: a composite shared between two code paths reports the cheaper path's
minimum.** `bench.h` already forbade this for the slowpath LEAVES -- "a shared accumulator would
let one slowpath sample move the fastpath's min with nothing in the table saying it had" -- and
`CALL_LOCKED` and `CALL_TOTAL` did it anyway. The first `esp32c6-wroom` capture showed
`CALL_LOCKED` at `n = 140000` against `CALL_MINT` at `n = 120000`, and its minimum of 1652 was
below the 1571 its own fastpath leaves already sum to: the 20000 slowpath calls of the donating
step, which park without ever reaching the mint, owned the minimum. **A composite smaller than its
own leaves is the tell, and matching `n` against a leaf only that arm executes is the check.** The
two arms now close different phases, and `endpoint_recv` gained the same treatment.

**A bracket count is a source fact and must be stated with the number it corrects.** 3.0.4 states
19, 9 and 6 for the three locked legs and 42 for the whole round trip, each confirmed by an `n`
column rather than asserted.

## 5. MEASURED: the fastpath works, and section 4.4's budget was wrong

Built for rv32imac and measured on `esp32c6-wroom`, same tree, same board, only an A/B knob
differing. Both captures reproduced byte-for-byte across independent flash cycles.

| payload | fastpath off | fastpath on | apparent delta |
|---|---|---|---|
| **8 B** | 55586 ns | **46998 ns** | -8588 ns (-1374 cycles) |
| **16 B** | 56581 ns | **48344 ns** | -8237 ns (-1318 cycles) |
| 32 B and above | unchanged | +175 ns | buffer form, not taken |

**The apparent delta lies, and correcting it is the point of section 4.10.** Both builds carry the
bench instrument, and the fastpath executes none of the call-side brackets. The instrument's own `n`
columns say how many: bracket executions fell by 760000 over 40000 fastpath calls, which is
**exactly 19.00 brackets per round trip** -- an integer, which is how the accounting is known to have
closed. At this board's calibrated 46.5 to 50 cycles per bracket that is 880 to 950 cycles of pure
instrument inside the apparent gain.

**Corrected: about 425 to 490 cycles, 6 to 7 percent end to end.** Two independent cross-checks
license the correction: the corrected off-side baseline lands at 6872 to 7014 cycles, inside the
6830 to 6940 that section 3.0.4 derived a completely different way; and the phase `n` columns
reconcile to the exact number of sub-20-byte calls.

### 5.1 Section 4.4 predicted ~1730 cycles attackable. We got about a quarter of it.

Stating that plainly rather than dressing it up. **The budget analysis was wrong, and the way it was
wrong is the transferable lesson.**

- **The ~1730 figure was an LX6 RESIDUAL, not a measurement, and residuals do not port.** It was
  whatever remained after subtracting named rows from a 4818-cycle round trip on Xtensa -- a
  WINDOWED ABI, where a syscall entry spills register windows, on a board with NO MPU. rv32imac has
  a flat 32-register file and a fixed 128-byte frame. There was never any reason for that leftover
  to describe this chip, and section 4.4 presented it as if there were.
- **On this board the round trip is dominated by what a fastpath may NOT touch**, exactly as section
  4.4's own floor argued but in a much larger proportion than it assumed. `MPU_APPLY` alone is 450
  cycles times two switches -- **900 cycles, 13.8 percent** -- and its sample count is IDENTICAL in
  both builds, confirming the fastpath removed no switch. Switch machinery that may not be skipped
  totals about 1428 cycles, 21.9 percent.
- **Only one of the round trip's THREE locked legs was attacked.** Section 3.0.4 established that a
  round trip holds the lock three times, not twice; the recv park and the reply still take the full
  generic path with two more trap round trips between them.

### 5.2 What phase 1 found, which was better news than expected

The rv32imac trap path is the structural twin of armv7m's -- the trap rewrites the return address to
a trampoline and returns into it on the caller's own stack, dispatching only afterwards -- so section
4.8's seam exists here unchanged. It is **cheaper** than armv7m: one frame format for every path, so
no split hardware and software frame and no floating-point conditionality; a single-word context; and
a switch that is a stack-pointer swap plus a deferred protection commit.

### 5.3 Where the remaining headroom actually is

- **`CALL_MINT_CAP` is 287 cycles -- more than half the entire measured gain, in one leaf.** The
  reserved per-thread reply slot of sections 4.6 and 4.10 is now clearly the highest-value next
  move, AHEAD of more frame surgery. That is a reversal of this page's earlier ordering and it is
  the measurement that reversed it.
- The stub copies the full register-form payload regardless of the requested length, worth about 30
  cycles; the kernel reads only what was asked for, so the zero-fill buys nothing.
- The buffer path pays a constant **+28 cycles** for the stub's two size compares. That is a real
  cost borne by every large-payload caller so that small ones need not choose a path, and section
  4.1 accepted exactly that trade when it ruled the caller never chooses.

### 5.4 The witness, because a green suite proved nothing

The fastpath and the buffer form answer IDENTICALLY by construction, so the existing suite passing
was not evidence the fastpath was ever taken. A dedicated arm now forces it: the server XORs its
reply rather than echoing it, so a byte-identical answer cannot pass by accident, and the assertion
was proven to bite by a negative control. `sim` 241, `qemu` armv7m 41, `qemu-riscv` 35, zero
failures; the selftest plan moves 104 to 105.

## 6. The instrument is too heavy at the source level, and that is a decision not a complaint

The bracket pairs reach four kernel files beyond `kernel/bench/`, and they sit in the hottest paths
by construction. **They cost nothing when compiled out and they cost readability and maintenance
always.** That second cost is the one that matters and it is accepted for M5, not defended.

What the in-tree instrument bought, so a replacement is judged against it rather than against
nothing: it found its own documented correction rule wrong by a factor of about 56; it found a
composite reporting a DIFFERENT code path's minimum; it found half the protection cost unbracketed
because that half runs in assembly; and it showed the fastpath delivering roughly a quarter of its
prediction. A replacement that cannot find those is not a replacement.

**The direction after M5 is external tooling.** A hardware trace path writes one word to a stimulus
port and lets the probe timestamp it, against the current mark, span, subtract and accumulate. The
fleet has a J-Link and `xmc4800` carries a J-Link OB, so the experiment is cheap.

Two constraints to size that experiment honestly:

- **ARM only.** Trace of that kind is a Cortex-M feature with a trace unit behind it, absent on the
  armv6m parts and on the RISC-V, RX and Xtensa backends. Every number on this page was taken on
  `esp32c6-wroom`, which such a path CANNOT reach, so a fleet-wide story would be two instruments.
- **It is not free of code either.** The tree's RTT path is a memory protocol and still needs a
  write per record. What moves off-target is the timestamp arithmetic and the accumulator, not the
  call site.

So the target is FEWER AND CHEAPER call sites with the arithmetic off-target, not an
instrument-free kernel.

**WHEN: settled during SMP or between SMP and the MMU work, not in M5.** SMP is when the question
gets its hardest input, because a per-core instrument must answer what a shared accumulator across
cores means, and the state inventory already rules the bench accumulators per-core. Deciding the
shape before that constraint is known would decide it twice.
