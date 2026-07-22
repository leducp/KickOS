<!-- SPDX-License-Identifier: CECILL-C -->
# The time base: a monotonic clock you can trust

> The conceptual chapter behind the "time" facet of the kernel model (Chapter 2).
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
quantum`"; a timed `sem_wait` is "arm for the nearest of my deadline and
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
- **Freezes on debug-halt / may stop in low-power.** A cycle counter that stops
  when the core stops mis-measures any interval that spans a halt or a deep idle.

### 2. A periodic core tick

ARM `SysTick` is the archetype: a core-domain, always-reliable, dead-simple
countdown that interrupts at a fixed rate. It is the FreeRTOS default and the
NuttX default for good reason -- as a *tick* it is excellent.

But as the *free-running now* a tickless kernel wants, it is a poor fit: it is
periodic by construction (fighting the tickless goal), and it is narrow -- 24
bits on Cortex-M -- so it cannot serve as a wide monotonic counter without the
same software-extension fragility. SysTick earns its keep as the opt-in periodic
tick (`CONFIG_SCHED_PERIODIC_TICK`), not as the default clock.

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
`arch/include/kickos/arch/arch.h`. The arch layer supplies a **weak** default;
a chip supplies a **strong** definition that overrides it when the arch default
is wrong for that silicon. This is the same weak-override seam the rest of the
arch layer uses (idle, MPU, blink), and it is what lets one chip swap the clock
source without disturbing the arch or the kernel above it.

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
authority: a chip that has a reliable free-running peripheral defines a strong
`arch_clock_now()` over it and leaves the weak DWT default unused. (QEMU targets
do the same for a different reason -- the model freezes or omits `CYCCNT` -- which
is the same lesson from the emulator side: the cycle counter is not a dependable
authority.)

### Per-arch realisations

Each ISA satisfies the requirements with whatever wide, reliable counter its
silicon offers. The shape is uniform; the peripheral differs.

| ISA / core         | Authoritative `now()` source                                   |
|--------------------|----------------------------------------------------------------|
| RISC-V (RV32IMAC)  | CLINT `mtime` -- wide, reliable, memory-mapped                  |
| ARM Cortex-M       | a free-running peripheral timer (the M0+ boards already do this)|
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

## Where to go next

- The exact, binding seam and the tickless scheduler mechanics:
  `../reference/architecture.md` (`arch_timer_arm` / `arch_clock_now`, and the
  "Tickless" section -- delta list, minimum-delta guard).
- Each chip's concrete counter (peripheral, rate, latch strategy): Chapters 4 and
  5 (per-ISA and per-chip tours) over `arch/*/chip/*`.
- The one-shot's other half -- how a deferred timer interrupt drives a switch on
  the way out of an ISR: Chapter 3.5, *Context switching and the silicon
  contract* (the deferred-switch axis).
