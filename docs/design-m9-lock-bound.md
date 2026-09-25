<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The kernel lock's own bound -- fair arbitration per backend

> **Status: LANDED.** M9.1, recorded 2026-09-22. The lock is a ticket on every shared-kernel
> backend, and the worst wait is derived per backend or the backend records that it cannot be.
> One backend records that it cannot be, and that is a result rather than a gap.

## What this stage was for

Before it, the kernel lock was a bare test-and-set retry loop on all three shared-kernel
backends: no ticket, no queue, no backoff, no bound. A core that lost a race lost it for a whole
critical section, and losing repeatedly was architecturally permitted forever. `klock.h` takes
the lock AFTER the interrupt mask, so a core spinning for it spins interrupt-masked and the whole
wait sits inside every core's interrupt-latency bound: an unbounded, unstated term in a budget
this project otherwise states carefully.

## One algorithm, three backends, three different bound statements

    g_next_ticket   drawn from once per acquisition by a read-modify-write
    g_now_serving   written only by the current holder, at release

    draw a ticket
    while (true)
    {
        if (acquire-load of now_serving == my ticket) { break; }
        kickos_doorbell_poll();
        <the backend's spin hint>
    }

**The turn test precedes the poll, and that order is load-bearing.** When the turn has come the
previous holder has already released, so no lock-holding initiator can be waiting on this core's
doorbell answer, and skipping the poll on that iteration strands nobody. When it has not come the
poll runs, which is the coupling `arch.h` requires of every acquisition and which the doorbell
selfcheck's second phase exists to witness.

**No ticket is stashed anywhere.** The release executes on the same core that drew, on every path
in this tree: `klock_enter` and `klock_leave` are a core-local pair, `klock_detach` sets `owed`
and the swap's `kickos_switch_unlock` runs on the same core, and the selfcheck and the parks
acquire and release straight-line. So only the holder writes `g_now_serving`, the release is a
load, an add and a release store, and the release path needs no per-core cell and no core
identity. That matters beyond tidiness: `arch_kernel_unlock` is a leaf whose frame is priced in
the xtensa trap red-zone record, and a core-identity call there would move a figure the whole
armv6m-class geometry is derived from. Re-derived against the new image, that figure is unmoved.

## The bound

    W = D + (N - 1) * (C + H + S)

| term | what it is |
| --- | --- |
| `N` | the kernel cores contending |
| `C` | the longest critical section, the longest depth-zero hold with its own wait taken out |
| `H` | the hand-off: the release store's visibility plus the successor's observing load, a coherence-fabric term rather than an ISA one |
| `S` | one doorbell service body a successor may already be inside when its turn arrives |
| `D` | the draw, and the only term that differs per backend |

**`(N - 1)` is a theorem whose hypothesis is one ticket per core**, which the interrupt mask is
what guarantees. It is measured: the `draw-queue` row never exceeds `N - 1` on any core in any
window, at two cores and at four.

**`S` IS THE TICKET'S OWN PRICE AND IS CHARGED ONCE PER HAND-OFF, NOT ONCE PER WAIT.** Under
test-and-set a core busy inside a doorbell service simply lost the race and cost nobody. Under a
ticket the lock is RESERVED for one named core, and if that core is inside `kickos_doorbell_poll`
when its turn arrives the lock sits idle and every later asker waits with it.

**An earlier form of this record charged `S` once for the whole wait and that was wrong.** The
argument was that only the immediate predecessor's release can hand the lock to a core that is
mid-poll, and that core is the asker itself. That is true of the asker's OWN hand-off and it is
not the wait: a wait contains every predecessor's hand-off too, and each of those can hand the
lock to a core that is not looking. So the gap is once per hand-off ahead of the asker, which is
where the `S` above sits.

**No measurement here could have caught it.** At two cores there is one hand-off and the two forms
are equal, and two cores is the only width any silicon in this project has. The error is visible
only at four and only by argument, which is why the bound is stated as a derivation and checked as
one.

**`S` is not removable by dropping the poll.** An initiator holding the lock and waiting on this
core's doorbell answer would never be answered.

### rv64imac -- the draw retries never, and the ISA says so

    D = one instruction plus the fabric's arbitration for N simultaneous AMOs

`amoadd.w` draws the ticket. The Zaamo section describes an AMO as atomically loading, applying
the operator and storing back, and defines **no status result and no failure encoding at all**.
The contrast is in the same chapter: Zalrsc states outright that a failing store-conditional
"does not write to memory, and it writes a nonzero value to rd", and needs a further section to
bolt an eventual-success guarantee onto constrained loops. Zaamo has no such section because it
has no such failure mode. **An `amoadd.w` that retired has performed.**

Measured: the `draw-retry` row reads `0/0/0` on every core in every window at four harts, maximum
included. The structural zero is confirmed and has no exception anywhere.

What remains is the memory system's arbitration for simultaneous AMOs to one word. The ISA does
not bound it and does not pretend to, so it is named here as a fabric term and not an ISA one.

### armv8a -- the bound has one free term, and it is a measured axiom

    D = R * d, where d is the four-instruction draw iteration

The modelled part is a Cortex-A53, ARMv8.0-A with no large-system extension, so there is no
atomic add and the draw is itself a load-exclusive/store-exclusive retry loop. `R` is
architecturally unbounded: the only guarantee is that the exclusives monitor may not be cleared
so as to prevent the forward progress of **at least one** of the threads accessing it, which
bounds the system and not the PE.

**What a capture has to bound**, stated so the measurement has a target: `R` is the maximum number
of store-exclusive failures a single core executes inside the draw loop during one draw, under
`N`-way simultaneous draw. It is a COUNT OF RETRIES PER DRAW and deliberately not a cycle figure,
which would fold `R` together with the instruction cost and the fabric and validate nothing about
the form of the bound.

Measured at four cores: `draw-retry` reads `0/0/1` aggregate and per core. **`R` is 1 under this
workload**, against a term the architecture refuses to bound at all.

**The draw loop's tightness is a correctness requirement, and the code before this stage did not
meet it.** The architecture guarantees forward progress for such a loop without the large-system
extension only when, between a store-exclusive returning a failing result and the retry of its
load-exclusive, there are no system register writes, exception returns, indirect branches or
branch-with-link instructions. The old acquire loop called `kickos_doorbell_poll` in exactly that
interval, so the one guarantee available had lapsed. The ticket removes it for free, the poll
having left the exclusive retry path entirely. **In a bench build one plain ALU instruction counts
attempts at the loop head**, which that list does not name, so the guarantee holds in both builds
and only the production loop is the bare four.

**No wait-for-event anywhere in this loop.** A physical interrupt is a wakeup event regardless of
the mask only for one of the two asynchronous-exception classes; an interrupt targeting the
current exception level with its mask bit set is the other, and is not a wakeup event. A waiter
under the mask this lock is taken behind could sleep through the doorbell it owes an answer to.

### lx6 -- no bound, and the reason is named rather than hedged

    D = R * d, R unbounded
    W = UNBOUNDED

**The unbounded step is the ticket DRAW and nothing else.** Once a ticket is held the wait is
`(N-1)*(C+H) + S` and is as analytical here as anywhere, with `N` of two.

**Why the ISA leaves it so.** The Xtensa summary specifies what the conditional store does, what
it orders, and how it reaches memory: as a read-compare-write transaction on the processor
interface bus, where external logic must then implement the atomic read-compare-write.
Arbitration among simultaneous such transactions from two cores is therefore a property of logic
outside the processor, and the ISA states nothing about it. **The phrase "forward progress" does
not occur anywhere in that document** -- no eventuality clause as RISC-V has for constrained
LR/SC loops, and no at-least-one-core note as the ARM architecture has. So this backend has not
merely no bound; it has no architectural lock-freedom of the draw either.

**And the part's own manual cannot supply one.** Every occurrence of "arbitration" in the ESP32
technical reference manual is either the I2C protocol's arbitration-lost condition or the Ethernet
DMA's transmit/receive scheduling. The instruction, the bus it goes out on and the transaction it
becomes occur zero times. **There is no document on this bench from which the bound could be
sourced**, which is a stronger statement than "a different document would be needed".

Measured on silicon, two cores: the `draw-retry` row reads `0/0/0` on both cores over 161973 draws
in one window, and at most 1 in a later one. The unbounded step did not fire.

**A ticket is still the right thing here, and the argument is about shape rather than size.**
Under test-and-set the unbounded event is "you were overtaken" and its unit is a whole critical
section. Under a ticket it is "your conditional store failed" and its unit is one five-instruction
retry. Its trigger rate falls too, from every spin iteration to every peer ACQUISITION, because
the ticket counter is written once per acquisition and never by a waiter: waiters spin on a plain
load-acquire and issue no bus transaction at all. Both improvements are structural and hold at any
contention level. **None of that is a bound and it may not be written up as one.**

**The load stays inside the draw loop on this backend**, against the obvious optimisation: the ISA
notes that the conditional store usually returns the current memory value, and that a few
implementations of the option return the bitwise complement of the compare register instead when
the store was not done. Hoisting is correct on most implementations and not on all.

## The interrupt mask: evaluated, and refused

**With a ticket, an interrupt taken mid-spin deadlocks the machine.** A handler reaching
`klock_enter` on a waiting core draws a SECOND ticket on a core that already holds one. The outer
context holds the earlier ticket and cannot run, being suspended in the exception frame;
`g_now_serving` walks up to that ticket and stops there forever, because the only code that
advances it is the outer context, which cannot resume until the handler returns, which cannot
happen until the handler's own later ticket is served. Unconditional, every backend, any width.
Under test-and-set the same nesting is merely an extra frame and an extra wait.

Put the other way round: **the `(N-1)` term is a theorem whose hypothesis is the interrupt mask**,
so opening it would destroy the bound this stage exists to establish.

**And the poll already buys nearly everything opening would buy.** Every device line's dispatch
reaches the scheduler, which takes the same lock, so an opened window is consumed by a handler
that immediately blocks on it one frame deeper. The one handler class that publishes and returns
without taking the lock is the doorbell service body, and the poll in the acquire loop already
runs it.

**The cost of the refusal, stated rather than hidden.** The lock wait stays inside every core's
interrupt-latency bound. The only ways to shrink that contribution are to shrink `W`, which is this
stage, and to shrink `C`, which is the stages after it.

## What the lock costs, measured

Emulator, five runs a preset, last report window, median over runs, idle box:

| | 1 core | 2 cores | 4 cores |
| --- | --- | --- | --- |
| `qemu-arm64` ns/sw | 6280 | 23818 | 51120 |
| `qemu-arm64` `lock-hold` p50/p99/max | 1280/3072/43993 | 6144/22528/354839 | 18432/57344/2926226 |
| `qemu-arm64` `lock-wait` p50/p99/max | absent, one core | 288/11264/349709 | 9216/36864/2904925 |
| `qemu-riscv64` ns/sw | 7276 | 20566 | 53501 |
| `qemu-riscv64` `lock-hold` p50/p99/max | 2560/7680/64100 | 12288/30720/670020 | 40960/180224/3999360 |
| `qemu-riscv64` `lock-wait` p50/p99/max | absent, one core | 576/8192/632200 | 15360/131072/3972280 |

**No board's `lock-hold` maximum is a critical section, on any ISA.** The `lock-site` line names
the return address of whatever held the outermost bracket at the release that set that core's
maximum. On every SMP preset and every arch it resolves to TRAP ENTRY: the syscall entry
trampoline and the IRQ path on armv8a, the supervisor vector, the syscall dispatch and the ISR
dispatch on rv64, the syscall entry and the dispatch on lx6. Those are callers waiting, not bodies
holding. At one core on `qemu-riscv64-bench` it resolves to the console emitter, which is the
M8.13 `esp32c6-wroom` finding on a second board.

**None of these figures may be used as a bound.** An observed maximum is a sample; the
distribution validates an analytical bound and does not replace it.

## The first shared-kernel silicon measurement

`esp32-wroom-benchsmp` is the LX6 microbench on both cores under one kernel. It exists because the
LX6 has no emulator, so it is the only path to a lock measurement on a part whose conditional
store has no derivable bound. Five captures, bit-identical across independent flashes:

    # cores: 2 arrived
    # smp sched: 2 core(s) in the scheduler
    cycle counter: 240000000 Hz
      throughput: 43742 ns/sw over 40000 switches
      lock-hold:  2560/7168/59801 cyc   (p50/p99/max, n=120093)
      lock-wait:   288/3072/54933 cyc   (p50/p99/max, n=160592)
      draw-retry:    0/0/0              (n=161973)
      draw-queue:    0/1/1

**It is the only multi-core configuration in this project with a live cycle counter.** All five
emulator presets declare `0 Hz`, which is why the entry envelope records the locked fraction as
absent on every multi-core configuration that exists. It forms here, and it is the first one:

    spans per switch  3.002
    cycles per switch 10498
    f                 0.732

**That fraction is not the single-core fraction and may not be tabled beside it.** Above one core
the hold bracket contains the wait, so this figure prices wait plus critical section where the
0.380 and 0.342 measured on single-core silicon price a critical section alone. `hold - wait` is
not derivable: the two rows are different populations, 120093 against 160592.

## The finding that outlives the stage

**Outermost lock acquisitions per switch scale with the core count, and the scaling is linear.**
Measured per core, on three ISAs, at three widths:

| cores | acquisitions per switch, per core |
| --- | --- |
| 1 | 2.0005 |
| 2 | 3.002 (lx6, silicon), 3.013 (armv8a) |
| 4 | 5.02 (armv8a), 4.98 (rv64imac) |

It is `N + 1`, and it is not a denominator artifact: at two cores each core takes 60055 and 60038
holds against 20008 and 20004 of its own switches, and the four-core per-core ratios are 5.33,
4.54, 4.86 and 5.45.

**So the lock traffic a switch costs grows with the width the lock exists to serve.** At one core
a switch pays two outermost acquisitions; at four it pays five, on every core. That term enters
the locked fraction directly and is why a multi-core fraction cannot be compared with a
single-core one even on the same part.

**The mechanism is not established here and is recorded as a hypothesis.** The shape matches one
acquisition for the core's own entry, one for the resume, and one per peer that asked it to
reschedule -- which would make the scaling the reschedule ask's, and would make it exactly what
per-core ready queues and per-pair rings are for. Attributing it needs a capture that counts
acquisitions by entry reason, which no instrument here takes.

## What changed meaning for a reader comparing captures across this stage

**`lock-wait` is not the same object before and after.** Before it measured barge contention, how
long a core lost races for; after it measures queue position times predecessor critical sections.
The row's one-line description stays literally true, so no gate notices, and two captures either
side of this stage are two different quantities.

**The resume path lost its barge.** A thread resuming on a core that must re-take the lock joins
the back of the queue rather than possibly claiming it immediately. That is the intended fairness
and it moves cost onto the switch path, so the switch-enclosing rows are expected to move.

## The doorbell gate's floor was calibrated per width, and then retired

`check_bench_doorbell.sh` held a round's per-core minimum to twice the raise floor measured in the
same burst (one and a half at one peer), to catch a bracket closing inside the raise. **No factor
survives a loaded host.** Under MTTCG the raising vCPU can be held up after the raise leaves for as
long as the peers take to service it: a probe counting rounds whose every answer was already in
when the raise returned read 62 to 64 of 64 on the failing bursts, and the gate failed 7 runs in
10 under 28 busy loops. The gate now counts rounds held, each peer's answer in the cells right
after the bracket closes, which catches the bracket moved above the wait on every idle run and
asserts nothing a contended emulator can take away.

## What this record does not establish

- **Nothing about the ticket's throughput cost.** The before captures and the after tree both
  exist; the paired campaign does not. A bench build also pays two distribution updates and one
  extra acquire load per acquisition that the pre-change tree did not, priced at about 2 us a
  switch at four cores, so a naive before-and-after difference charges the lock for the
  instrument.
- **Nothing at more than two cores on silicon.** The one silicon vehicle is a two-core part.
- **No bound on lx6.** That is the deliverable there and not a gap in it.
