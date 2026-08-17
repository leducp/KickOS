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

### 3.0.2 D1 IS STRUCTURALLY UNREACHABLE, and that decides the donation question

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

**The instrument is far more expensive on this board than on the LX6**: the round trip goes from
4736 cycles uninstrumented to 7859 instrumented, where `PH_NULL` reads 1. The DWT sits in the PPB
and an M7 access there is not a core-register read, and the brackets are optimisation barriers in a
path the compiler otherwise schedules freely. So teensy figures are for RATIOS and nesting; the LX6
carries the cheaper absolute numbers.

### 3.0.1 The Amdahl number, at last

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

**Refuse.** D1 is admissible only when the caller outranks the server, and a caller that outranks
its server never finds a parked receiver in sustained traffic, because it re-calls before the
server can loop back and park. So the fastpath refusing donation costs nothing measurable and
removes a branch plus a `sched::set_prio` from the hot path.

**The harder consequence is about WHICH path the fastpath should be.** The rendezvous fastpath
serves equal-priority peers; the slowpath serves the priority-inverted case, which is the one with
latency to save. A work fastpath that only handles the parked-receiver rendezvous optimises the
traffic that has no inversion to fix. Whatever section 4 eventually specifies has to say what it
does for a high-priority client, or it is aimed at the wrong half.
