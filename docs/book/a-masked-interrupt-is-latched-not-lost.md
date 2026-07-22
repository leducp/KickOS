<!-- SPDX-License-Identifier: CECILL-C -->
# A masked interrupt is latched, not lost

> Where an interrupt that arrives while its line is masked has to go, and why the
> only answer that keeps a userspace driver's `wait` honest is *latch it and deliver
> it at unmask* -- never drop it. This chapter builds on Chapter 8 (the blocking
> substrate: one wait/wake primitive) and the tier-1 IRQ model, and points into
> `../reference/invariants.md` (`isr-mask-then-wake-wait-rearm`) and
> `../reference/porting.md` (the per-controller `arch_irq_*` contract) for the exact
> rules -- this chapter explains why they read the way they do.

## The window nobody opened on purpose

A tier-1 interrupt in KickOS is not handled in interrupt context. The first-level ISR
does the least it can: it **masks** the line so it cannot re-fire, posts the line's
bound notification (a semaphore), and returns. The real work happens later, in the
driver thread, which wakes from `irq_wait`, services the device, and loops back to
`irq_wait` -- and that next `irq_wait` is what **re-arms** (unmasks) the line. The
design is deliberate: the driver is an ordinary unprivileged thread, scheduled by
priority like any other, and the line stays masked for exactly as long as it takes that
thread to run.

That "exactly as long as it takes" is a window. Between the ISR masking the line and
the driver's next `irq_wait` unmasking it, the line is masked -- and the device does
not know that. A second event can arrive in that window. Nothing in the design opened
this window on purpose; it is the unavoidable cost of moving the handler out of
interrupt context. The driver cannot close it -- the window exists *because* the driver
has not run yet.

So the kernel has to decide what a raise that lands in the masked window *means*. There
are only two answers, and one of them is a quiet lie.

## Drop is the tempting wrong answer

The tempting answer is: drop it. The line is masked; the driver is already dealing with
one event; a raise that arrives now can just be discarded, and the world stays simple.
It is simple to implement -- on a controller with a software mask you check the mask bit
in the raise path and return early -- and for a while it even looks correct, because the
tests that exercise it are written on hardware fast enough that the driver always
re-arms before the second raise lands.

It is a lie because the tier-1 contract makes a promise the drop breaks. The promise is
that the semaphore *is* the event record: a driver that calls `irq_wait` receives every
event, in order, one wake per event, and never has to poll the device to find an
interrupt the kernel forgot. `wait` is lossless. Drop-on-masked silently voids that
promise for any event unlucky enough to fall in the window -- and the failure is not a
dropped byte or a late wake. It is a driver blocked in `irq_wait` forever, because the
wake that would have freed it was thrown away while the line was masked, and nothing
will ever raise it again.

## Why drop *looks* safe: the level source hides the bug

Drop-on-masked survives longer than it should because the most common interrupt source
forgives it. A **level** source -- most on-chip peripherals -- holds its interrupt
output asserted until the driver clears the device's flag. Its line is
`status AND enabled`. If a level event arrives while the line is masked, the source is
still asserted when the line is unmasked, so the controller re-latches pending from the
still-high input: the event was not preserved, but it *re-presents itself*, and the
driver sees it anyway. Drop and re-present are indistinguishable from the driver's seat.

The forgiveness ends the moment the source is **edge- or pulse-shaped**: a one-shot with
nothing to re-present. A software-generated interrupt, an inter-core doorbell, a
mailbox with a pulse output, an edge-latched peripheral line -- each raises once and is
gone. Drop it during the masked window and there is no still-asserted level to
re-latch from. The event exists nowhere. The driver's next `wait` blocks on a wake that
will never come.

This is the old distinction every interrupt controller has always drawn between level
and edge triggering, arriving through the back door. Drop-on-masked is secretly a
*level-only* policy wearing the clothes of a general one. It works until the first
pulse-type source shows up -- and a kernel that offers a generic tier-1 IRQ API to
userspace drivers is promising to carry pulse-type sources.

## Latch is the answer, and the hardware already agrees

The correct contract is the other one: **latch**. A raise that lands on a masked line is
remembered and delivered through the normal ISR path the instant the line is unmasked.
Nothing is dropped; the event is merely *deferred* to when the driver is ready for it.

The remarkable part is that this is what the silicon already does. An interrupt
controller's pending bit is a latch: setting pending on a disabled line does not fire an
interrupt, but the bit stays set, and enabling the line delivers it. The Cortex-M NVIC
latches a pending write independently of the enable bit; the RISC-V PLIC latches
pending and will only let you consume it by claiming it -- there is no operation that
*discards* a pending you did not handle. On these controllers the drop policy is not
just wrong, it is *unimplementable in hardware*: to make drop-on-masked work you have to
hand-build it in software on every backend, adding a mask-check to the raise path that
fights what the controller natively does. The latch contract asks for none of that. It
lets the hardware behave the way it was built to, and it lets the software backends
carry a single "pending while masked" bit that mirrors it.

The two latches stack cleanly. The controller's pending bit is the *hardware* latch that
bridges the masked window: it holds the fact "an event arrived" across the interval the
line is disabled. The semaphore is the *software* latch that makes `wait` lossless: when
the line is unmasked, the held pending fires the normal ISR, which posts the sem, and
the driver's `wait` returns. A counting semaphore even absorbs the harmless case where
the post beats the wait. The event flows hardware-latch to software-latch to driver,
and no link in that chain is allowed to be `unmask` reaching in and posting the sem
itself -- redelivery must go through the real ISR path, so the same masking and
re-arm bookkeeping runs as for any other interrupt.

## How many events is a burst? Exactly one

If two raises land while the line is masked, how many wakes does the driver get? One. A
masked line carries a single fact -- "there is an event waiting" -- not a count. A
second raise onto an already-pending masked line changes nothing: the pending bit is
already set. This is **one-deep coalescing**, and again it is what the hardware does for
free: a pending bit is one bit. The API inherits the semantics of the latch it is built
on, rather than promising a per-event queue the controller cannot back.

One-deep is the honest contract because it is the one the driver can act on. When the
driver wakes, it services the device to quiescence -- it does not assume one wake means
exactly one unit of work. A level source may have several units asserted behind a single
pending bit; a re-armed line that fires again gives the next wake. Coalescing to one and
draining on each wake is the shape that works for every source type without the kernel
having to count.

## Discard still exists -- as its own deliberate act

Latching is the rule, but there is exactly one time the kernel *does* want to throw a
pending away: at **first-arm**, when a driver binds a line it did not previously own. A
line can carry a pending bit from before any driver existed -- boot-ROM leftovers, a
raise that predates registration -- and that is not the new owner's event. Delivering it
would phantom-wake the driver's very first `wait` with an event it never earned.

The lesson from the drop story is *why* this discard must be a separate primitive rather
than a behavior folded into `unmask`. The moment discard hides inside `unmask`, every
re-arm silently destroys masked-window events, and you are back to drop-on-masked with a
nicer name. So the discard is its own explicit call -- clear the pending, *then* enable
-- used at exactly the two places it belongs: first-arm garbage, and (a future refinement
for level sources) a driver that has serviced its device and wants to drop the
controller's now-stale latch before re-arming. Splitting the discard out is what lets
`unmask` mean *preserve* everywhere else, which is the whole point. See
`../reference/porting.md` for the primitive's per-controller shape and the exact
first-arm ordering (arm the consumer, discard, then enable -- in that order, so a
preserved pend cannot fire against a half-built handler).

## The rule

Masking an interrupt line defers the interrupt; it does not un-happen it. A raise that
lands on a masked line is a real event that arrived at an inconvenient moment, and the
kernel's job is to hold it until the driver is ready -- latch it, coalesce a burst to
one, deliver it through the normal path at unmask. Drop it and you have told a userspace
driver that `wait` is lossless while quietly making it a poll. The masked window is
inherent to handling interrupts in thread context; the driver cannot close it; so the
kernel must never lose what falls into it.
