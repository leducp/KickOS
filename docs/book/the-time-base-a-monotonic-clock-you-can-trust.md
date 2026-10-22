<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The time base: a monotonic clock you can trust

> The conceptual chapter behind the "time" facet of the kernel model ([Chapter 2](kernel-model.md)).
> It teaches *what* a monotonic time base is, *why* a tickless kernel's every
> timed decision rests on it, and the checklist a porter runs before trusting a
> hardware timer as the authoritative clock. Each chip's concrete counter (which
> peripheral, at what rate) is a section of Chapters 4/5 and the exact seam lives
> in the arch clock code. Points into `../reference/architecture.md` (the
> `arch_clock_now` / `arch_timer_arm` seam and the tickless scheduler) -- this
> chapter explains, the reference binds.

## What a monotonic time base is

A scheduler needs to answer two questions about time, and only two:

- **"What time is it now?"** -- a free-running counter that only ever moves
  forward, read cheaply, at any instant. Call it `now()`.
- **"Wake me at time T."** -- a programmable one-shot that raises an interrupt
  when the counter reaches a chosen deadline.

That pair -- a free-running *now* plus an arm-able *next-event* -- is the whole
time base. Everything the kernel does with time is built from it: a `sleep(d)`
is "arm the one-shot for `now() + d`"; a round-robin slice is "arm for `now() +
quantum`"; a timed semaphore wait is "arm for the nearest of my deadline and
whatever was already pending." When an event fires, the kernel readies whatever
was waiting on the earliest deadline, then re-arms the one-shot for the next.

This is the **tickless** model, and it is worth contrasting with its opposite.

*Further reading: Tanenbaum, Modern Operating Systems, ch.2 (scheduling) and the
clock-driver material in the I/O chapter.*

### Tickless vs the periodic tick

The traditional model is a **periodic tick**: a timer interrupts at a fixed rate
(say 1 kHz), and on each tick the kernel decrements sleep counters and checks
whether any slice expired. Time is quantised into tick periods; "now" is a tick
count. It is simple and it is what FreeRTOS ships by default (the SysTick-driven
`xTaskIncrementTick`), and what NuttX uses in its default configuration.

Its costs are the reason KickOS does not use it. A periodic tick interrupts even
when nothing is waiting -- pure overhead and a wakeup source that fights every
low-power idle. Its resolution is bounded by the tick period, so a "10 us sleep"
rounds up to a whole tick. And a count-on-each-tick "now" only advances when the
handler runs, so a missed or delayed tick loses time outright.

A tickless kernel instead reads a real free-running counter for "now" and arms
the one-shot only for the *actual next* deadline. Idle with nothing pending means
**zero** timer interrupts. Resolution is the counter's, not a tick period's. (See
`../reference/architecture.md`, the "Tickless" section, for the delta-list and
minimum-delta-guard details.)

The load-bearing idea, and the reason this whole chapter exists:

> **Every sleep, every round-robin slice, every join or timed-wait deadline is
> computed against `now()`.** The scheduler is exactly as trustworthy as its
> clock. A clock that reads *too high even once* computes a deadline that has
> already passed for every currently-armed wait -- so the one-shot is set into
> the past, or worse, the kernel believes all pending deadlines are still far in
> the future and arms nothing. Either way every timed thread is stranded: the
> system goes idle and never wakes.

## Why: a bad "now" is a system-down, not a glitch

A miscomputed pixel is a glitch. A clock that jumps is a class apart, because
*time is the input to every scheduling decision at once*. Consider a `now()` that
momentarily reads far higher than reality -- a single reading that leaps by, say,
2^32 counter units.

- Threads sleeping until `T` are compared against a `now()` that already exceeds
  every `T`. They are all "overdue" -- or, if the jump is re-imported into the
  re-armed deadline, the next one-shot is programmed for `bogus_now + delta`,
  which is a time the *real* counter will not reach for many seconds (or ever,
  before its own wrap).
- The one-shot fires far in the future, or not within any bounded time. No
  thread is readied. The run queue drains to the idle thread. The idle thread
  waits for an interrupt that has been scheduled for the wrong epoch.
- Nothing has crashed. No fault is taken. The board sits in `WFI` looking
  healthy, and stops responding. From the outside it is a hang; from the inside
  every invariant "held."

This failure mode is silent, it is total, and it is intermittent (it needs the
clock to misbehave, which may be one reading in millions). That combination --
catastrophic and hard to reproduce -- is exactly why the clock deserves a chapter
of its own and a vetting discipline, not a casual "grab whatever counter the core
offers."

## How one could do it: the design space

A porter arriving at a new chip has three broad families to choose from for the
authoritative `now()`. Laid out honestly, with the trade a porter reasons from:

### 1. A core cycle counter

ARM `DWT_CYCCNT`, Xtensa `CCOUNT`, RISC-V `rdcycle`. The register is *right
there* in the core, free (no peripheral to claim), and high resolution (one
count per CPU cycle).

The costs are structural, not incidental:

- **Often 32-bit.** At a 120 MHz core a 32-bit cycle counter wraps every ~36
  seconds. "Now" must then be a software-extended 64-bit value -- and that
  extension is the fragile pattern this chapter is really about (below).
- **Sometimes in the core debug power domain.** ARM's DWT is part of the debug
  block. That domain can be gated, can require a debugger-set enable, and -- the
  sharp edge -- a read can *alias a neighbouring register* and return a value
  that has nothing to do with elapsed time. A clock authority that can return
  garbage is not an authority.
- **Sometimes optional / trapping.** `rdcycle` from an unprivileged context may
  trap if the counter is not enabled (`[m|s]counteren`), and the cycle extension
  can be absent or restricted on a given implementation.
- **Freezes on debug-halt and in low-power.** A cycle counter that stops
  when the core stops mis-measures any interval that spans a halt or an idle. ARM's
  `CYCCNT` counts processor clock cycles, and on an STM32F411 the `WFI` sleep stops
  that clock, so the counter stands still exactly while the kernel idles. A
  debugger can keep the clock running in sleep (ST's `DBGMCU_CR.DBG_SLEEP`), so the
  counter behaves one way on the bench and another way in the field.

### 2. A periodic core tick

ARM `SysTick` is the archetype: a core-domain, always-reliable, dead-simple
countdown that interrupts at a fixed rate. It is the FreeRTOS default and the
NuttX default for good reason -- as a *tick* it is excellent.

But as the *free-running now* a tickless kernel wants, it is a poor fit: it is
periodic by construction (fighting the tickless goal), and it is narrow -- 24
bits on Cortex-M -- so it cannot serve as a wide monotonic counter without the
same software-extension fragility. SysTick earns its keep as the opt-in periodic
tick (`CONFIG_KICKOS_SCHED_PERIODIC_TICK`), not as the default clock.

### 3. A memory-mapped peripheral timer

K64F PIT, RX CMTW, XMC CCU4, ESP TIMG, the RISC-V CLINT `mtime`. A dedicated
timer peripheral on a known bus clock.

- **Reliable and outside the debug domain** -- it runs whether or not a debugger
  is attached and does not alias other registers.
- **Can be wide** -- 64-bit natively (CLINT `mtime`), or two 32-bit channels
  chained into a 64-bit free-runner (the K64F PIT approach), so wrap is a
  non-issue for any realistic uptime.
- **Free-running and one-shot-capable** in the same block.
- **Cost:** it consumes a peripheral, and it needs a few lines of setup (enable
  the clock gate, program the mode, derive the rate from the clock tree). That is
  the entire price.

A comparison a porter can reason from:

| Source            | Domain / reliability     | Width           | Free-running | Cost                    |
|-------------------|--------------------------|-----------------|--------------|-------------------------|
| Core cycle count  | often debug/gateable     | often 32-bit    | yes          | free, but fragile reads |
| Periodic core tick| core, reliable           | narrow (24-bit) | no (periodic)| simple, but not tickless|
| Peripheral timer  | normal, reliable         | up to 64-bit    | yes          | one peripheral + setup  |

## What KickOS chose, and why

KickOS is tickless, so it needs a genuine free-running *now*, not a tick count.
Weighed against the design space, that points at a **free-running peripheral
counter** for `arch_clock_now()`, paired with the arch one-shot behind
`arch_timer_arm()`. The peripheral wins because it is the only family that is
simultaneously wide, reliably readable, and outside the debug/low-power domains.
The few lines of peripheral setup are a cheap premium against a silent
system-down.

That decision imposes concrete **requirements on any candidate timer** a port
proposes as the authoritative clock:

- Monotonic and free-running -- never reloaded or reset in normal operation.
- Wide enough that wrap is not a routine event, or hardware-latched so a wide
  read cannot tear.
- Readable at any instant without gating, and returning *its own* value (never an
  aliased neighbour).
- Outside the debug power domain (independent of debugger state).
- Driven by a known, stable input rate derived from the clock tree.
- Not shared with a driver that could reprogram it.

**The f411 example.** The STM32F411 shows the pairing on real silicon. The
one-shot is the core's own SysTick: the kernel arms it for the next deadline and
re-arms it at every new one, and a deadline beyond its 24-bit range simply fires
early and is re-armed for the rest. The clock is a different timer, TIM2, a
32-bit counter that runs from boot and is never reloaded. The two cannot be one
timer. A counter that is reloaded at every deadline loses the time it had already
counted, so it cannot also say what time it is. And at the 84 MHz core clock,
SysTick's 24 bits wrap about every 0.2 seconds, so extending it to 64 bits would
take an interrupt five times a second, which is the periodic tick again. TIM2's
32 bits wrap about every 51 seconds, and its overflow interrupt folds each wrap
into the 64-bit count. The core's cycle counter was the other candidate, and the
worked example below shows why it lost.

The next section turns those requirements into the vetting checklist -- and first
names the failure shape they exist to catch.

## The fragility pattern and the vetting checklist

### The dangerous shape

There is one recurring construction behind clock-jump system-downs:

> **A narrow, fast-wrapping counter plus a software wrap-extension.**

The code is seductively simple. Keep a 32-bit hardware counter and a software
"high" word; on each read, if the new low reading is less than the last one,
assume a wrap happened and increment high:

```
// The fragile pattern -- DO NOT trust this as an authoritative clock.
static uint32_t g_last_low; // last low word seen
static uint32_t g_high;     // software-extended high word

uint64_t now_extended(void)
{
    uint32_t low = read_hw_counter(); // 32-bit, wraps fast
    if (low < g_last_low)             // "it went backwards, so it wrapped"
    {
        g_high = g_high + 1;
    }
    g_last_low = low;
    return ((uint64_t)g_high << 32) | low;
}
```

This construction fails two independent ways, and both produce the
strand-everything jump from the "Why" section:

- **(a) An unreliable read.** If the source can return an aliased value -- a
  debug-domain counter momentarily reading a neighbour register -- then one bad
  *high* low-reading followed by a normal one looks exactly like a
  backwards-step. `g_high` increments on a wrap that never happened, and `now`
  leaps by 2^width permanently. One garbage read, one phantom wrap, a clock that
  is seconds ahead forever.
- **(b) A missed read.** The `low < g_last_low` test can only detect *one* wrap.
  If no read happens for longer than a full wrap period (a long critical section,
  a deep idle, an interrupt storm), the counter wraps twice and the second wrap
  is invisible -- `low` comes back *higher* than `g_last_low`, no increment, and
  `now` silently loses 2^width. The mirror image of (a): time falls behind, and a
  deadline computed against it is set far too late.

Both failure modes vanish if the counter is wide enough not to wrap in practice,
or hardware-latched so the read is atomic and truthful. That is why the checklist
below pushes hard toward wide, reliable, hardware-latched counters and treats
software extension as a last resort with preconditions.

### Extending in software, when you must

When a part offers nothing wider than 32 bits, the extension can still be made sturdy.
The fragile version above decides that a wrap happened by comparing two readings, so
its state comes from the readings, and one bad reading stays in the high word forever.
The sturdy version takes that decision away from the readers:

- **Only the overflow interrupt advances the high word.** The counter's own overflow
  flag says a wrap happened, so no reading is ever interpreted as one. The interrupt
  clears the flag and advances the word with interrupts masked, so a reader that
  interrupts it never sees the flag cleared and the word not yet advanced.
- **A reader writes nothing.** It reads the high word, then the counter, then the high
  word again, and starts over if the high word changed in between. The two halves it
  returns were then valid at the same moment.
- **A reader covers the one remaining window itself.** If interrupts are masked when it
  reads, the counter may have wrapped while its overflow interrupt is still waiting to
  run. The reader checks the timer's pending overflow flag: when the flag is set and
  the counter reads low (in the lower half of its range), the high word is one
  behind, and the reader adds it.

```
// The sturdy extension: the overflow interrupt owns the high word.
static kickos::Atomic<uint32_t> g_high; // written by the overflow interrupt only

void overflow_isr(void)
{
    uint32_t const saved = mask_interrupts();
    clear_overflow_flag();
    g_high.store(g_high.load() + 1);
    restore_interrupts(saved);
}

uint64_t now_extended(void)
{
    while (true)
    {
        uint32_t high = g_high.load();
        uint32_t low = read_hw_counter();
        bool const pending = overflow_pending();
        if (g_high.load() != high)
        {
            continue; // the interrupt ran meanwhile: read again
        }
        if (pending and low < 0x80000000u)
        {
            high = high + 1; // wrapped, and its interrupt has not run yet
        }
        return ((uint64_t)high << 32) | low;
    }
}
```

The loads must be real loads every time, which is why the high word is an atomic
rather than a plain variable: a compiler is otherwise free to read it once and loop
on a stale copy. No hardware barrier is needed on one core, since a core sees its own
stores in order.

What this buys: a bad reading produces one wrong value and is never latched, a
reader running at any interrupt priority is safe because readers share no state
and the interrupt's two stores are one step to them, and no reader has to mask
interrupts. Only the overflow interrupt masks them, for two stores. Failure (b) still applies to the interrupt
itself, more strictly: the "counter reads low" test is only right if the overflow
interrupt is never kept waiting for half a wrap period or more.

### The checklist

Run this against a candidate timer *before* trusting it as `arch_clock_now()`:

1. **Power domain.** Is it in the debug domain or a gateable low-power domain --
   can it stop, or return aliased reads when the domain is off? If yes, reject it
   as the authoritative clock. (This is what rules out the ARM DWT cycle
   counter.)
2. **Width and wrap.** 32 bits wraps fast; a software 64-bit extension is fragile
   per the pattern above. Prefer a wide (>= 48/64-bit) or hardware-latched
   counter. If you *must* extend in software, you owe two guarantees: reads occur
   comfortably within one wrap period (defeats failure (b)), *and* the source is
   reliable (defeats failure (a)). Missing either, do not extend.
3. **Read atomicity.** A wide counter read as two 32-bit words can **tear**: the
   high and low halves are sampled at different instants, so a wrap between the
   two reads yields a value from neither. Use a hardware latch -- a lifetime-timer
   register that latches the low word when you read the high word -- or a
   verify-read loop (read high, read low, re-read high; retry if high changed).
4. **Free-running and unowned.** Is it monotonic, never reloaded, and not shared
   with another user? A driver that reprograms "your" counter re-imports exactly
   the jump you are trying to avoid. The clock's counter must belong to the
   kernel alone.
5. **Known, stable input rate.** Derive its Hz from the clock tree explicitly,
   and *couple* that derivation to the clock-tree setup so a later retune of a
   divider cannot silently rescale time out from under the scheduler. (Deriving
   the rate from the same constant that programs the divider is the mechanism.)
6. **Debug-halt and low-power behaviour.** Does it freeze on a debugger halt (so
   intervals measured across a breakpoint are wrong -- a trap for halted
   inspection), and does it keep running in `WFI`/deep idle (so a sleep that
   spans idle still expires)? Know the answer for the chip; the authoritative
   clock must keep time across idle.

An honest note on effort against payoff: this vetting is a one-time cost per
chip, paid by the porter, and it buys immunity from a failure that is otherwise
catastrophic and nearly impossible to reproduce. It is the cheapest insurance in
the port.

## How it is done in the code

### The seam

`arch_clock_now()` (monotonic nanoseconds) and `arch_timer_arm()` /
`arch_timer_disarm()` are the entire kernel-facing time seam, declared in
`arch/include/kickos/arch/arch.h`. The optional seams around it (idle, MPU,
blink) each carry a fallback body in a translation unit of its own, which a
chip's own definition displaces. Whether the clock gets one is decided by the
checklist above, per ISA: where the core offers a counter that passes it, the
arch may supply the clock itself (ARMv8-A's architected counter `CNTPCT_EL0`,
64 bits wide and outside the debug block, so there is no wrap to extend); where
the only core-local candidate fails it -- ARMv7-M, whose only wide-enough counter
sits in the debug domain -- there is deliberately none, and a chip that forgets
`arch_clock_now()` fails to link. That is the point of leaving it out: a porter
cannot skip the vetting silently, or inherit a clock nobody vetted. Either way
the seam is what lets one chip swap the clock source without disturbing the arch
or the kernel above it.

### The worked example: a cycle counter is the wrong authority

Teach this as a principle, not a changelog:

> **A cycle counter in the core debug power domain is the wrong authority for a
> scheduler clock.**

Follow it through the pattern. The ARM DWT `CYCCNT` is a 32-bit counter in the
debug block -- so it trips checklist item 1 (power domain) *and* item 2 (narrow,
forcing the software extension of failure modes (a)/(b)). A debug-domain read
that aliases a neighbour delivers exactly the one-bad-high-reading that failure
(a) turns into a permanent 2^32 jump; that jump strands every timed wait per the
"Why" section. The fix is not to harden the extension -- it is to change the
authority: on ARM the clock is a per-chip contract, and each chip names a source that
meets the checklist rather than inheriting the core's, while the `CYCCNT` read is
demoted to the telemetry timestamp seam
(`arch_trace_now`), where a wrap costs a mislabelled trace rather than a
stranded thread. (An emulated target goes one step further and displaces even
that telemetry read, because the model freezes or omits `CYCCNT` -- the same
lesson from the emulator side: the cycle counter is not a dependable authority.)

What "meets the checklist" admits is wider than one kind of peripheral, and the
emulated boards are the instructive case. A board whose whole purpose is to exercise
the kernel under an emulator can answer `arch_clock_now` from the *host*, through the
semihosting clock the model already provides: no counter is programmed, no divider is
derived, and the granularity is 10 ms rather than nanoseconds. That is a legitimate
answer because the checklist asks for width, for a reading that does not stop when the
core does, and for a source outside the debug block, and not for a particular unit. The
coarse tick is honest about what an emulated board can measure anyway. It does drag one
requirement into the open that a
real peripheral satisfies for free: a host-provided reading can *glitch*, so the seam
clamps it against the last value it returned, since a single low reading would regress
the clock and a single bogus high one would strand every armed sleeper. The clamp is a
read-modify-write on a 64-bit variable that both thread and interrupt context touch, so
it runs with interrupts masked; a torn store latched by the clamp would jump the clock
forward permanently, which is failure (a) arriving by another road.

### Per-arch realisations

Each ISA satisfies the requirements with whatever wide, reliable counter is in
reach. The shape is uniform; the source differs.

| ISA / core         | Authoritative `now()` source                                   |
|--------------------|----------------------------------------------------------------|
| RISC-V (RV32IMAC)  | CLINT `mtime` -- wide, reliable, memory-mapped                  |
| ARM Cortex-M       | a free-running peripheral timer, or the host's semihosting clock on an emulated board |
| RX (RXv3)          | CMTW compare-match unit on a derived PCLKB rate                |
| Xtensa LX6         | a wide peripheral timer, chosen over the narrow core `CCOUNT`  |

These name the *kind* of source, not a claim about which chips are finished. The
requirement is the durable thing; the exact per-chip contract (which unit, the
register addresses, the derived Hz, the latch or verify-read choice) lives in the
chip code under `arch/*/chip/*` and is summarised in `../reference/architecture.md`
(the `arch_clock_now` / `arch_timer_arm` seam). One chip's setup is worth reading
as a template: the K64F chains two 32-bit PIT channels into a 64-bit free-runner
and derives the PIT rate from the same bus-clock constant that programs the
divider -- checklist items 2, 3, and 5 in one place.

### Reading the clock without asking the kernel

A thread asks for the time often: a timeout it computes itself, a measurement, a stamp on a log
line. Where the clock exists only behind `arch_clock_now()`, each of those reads is a system call,
a trap into the kernel and a return, which on a fast core costs far more than the counter read
it wraps. Where the counter behind the clock is a register of the core, the core can usually let
unprivileged code read it, and then a thread computes `now` itself. Three conditions make that
safe to offer.

- **The read is opened on every core before a thread runs there.** The opening is one privileged
  register write at bring-up: ARMv8-A's `CNTKCTL_EL1`, RISC-V's counter-enable registers, x86's
  `CR4.TSD` cleared. A core that misses it does not answer a wrong time; the read traps and the
  thread's task ends, which is loud.
- **The thread does the kernel's arithmetic on the kernel's counter.** Two clocks that disagree
  by one tick are two clocks, and a deadline taken from one and kept by the other fails at a
  boundary. So the rate the thread scales by is the kernel's own: a register user mode can read,
  a constant the chip description states, or, where only the kernel knows it because it measured
  it at boot, a value the kernel hands each program before any of its application code runs.
- **The kernel trusts nothing the thread read.** Every deadline a thread passes the kernel is a
  relative timeout, never an instant, so a wrong reading can only hurt the thread that made it.

For the thread, the cost changes and the meaning does not: the same values at the same
resolution. One consequence is easy to miss. A cooperative cancel lands when its target next
enters the kernel, and a loop that only reads a user-mode clock never enters, so it is no longer
a place such a cancel can land; a forced cancel still reaches it. And one thing is given away: the
counter becomes a timing source every thread on that core can read. Because the opening is a
per-core register, a kernel that wants to ration time can close it again for a thread at each
switch, for one register write.

Where the clock is a memory-mapped peripheral behind the protection unit, or the host's clock
reached through the emulator, the read stays a system call. The function a program calls is the
same either way.

## Where to go next

- Which arches let a thread read the clock itself, and what a port owes for it:
  `../reference/porting.md` ("The clock a thread reads itself").
- The exact, binding seam and the tickless scheduler mechanics:
  `../reference/architecture.md` (`arch_timer_arm` / `arch_clock_now`, and the
  "Tickless" section -- delta list, minimum-delta guard).
- Each chip's concrete counter (peripheral, rate, latch strategy): Chapters 4 and
  5 (per-ISA and per-chip tours) over `arch/*/chip/*`.
- The one-shot's other half -- how a deferred timer interrupt drives a switch on
  the way out of an ISR: [Chapter 3.5](context-switching-and-the-silicon-contract.md), *Context switching and the silicon
  contract* (the deferred-switch axis).
