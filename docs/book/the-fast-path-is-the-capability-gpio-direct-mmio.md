<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The fast path is the capability: why a chip-select toggle is direct MMIO, not a syscall

> A microkernel's instinct is to route every privileged act through a syscall, so the kernel
> can check it. This chapter is about the one place that instinct is wrong: a pin toggled
> inside a bus transaction. The resolution turns on seeing that a granted MMIO window *is*
> the authority -- the capability is the fast path, and a syscall on top of it only adds
> cost the check has already paid for. Along the way it splits a pin's two jobs (configure
> once, toggle often) and meets the same per-chip isolation ceiling Chapter 3.7 drew, now at
> pin granularity. Points into `../reference/architecture.md` ("Memory domains" -- the
> peripheral-MMIO grant) for the exact contract.

## A pin has two jobs, on two completely different clocks

Before deciding how software should touch a GPIO pin, separate what "touch a pin" means,
because the two meanings could not be further apart in how often they happen.

- **Configure the pin.** Choose its function (is this ball a GPIO, or the SPI clock, or a
  UART TX?), its direction, its drive strength. This is *pin-mux*. It happens **once**, at
  bring-up, and never again while the system runs.
- **Toggle the pin.** Drive it high, drive it low, read its level. For an LED this is rare
  and lazy. For an SPI chip-select (CS) it is the **hottest** thing in the driver: the CS is
  asserted before the first bit of a transfer and released after the last, on **every single
  transaction**, potentially hundreds of thousands of times a second.

A design that treats these as one operation -- "GPIO access" -- will get one of them badly
wrong. Configuration is rare, dangerous, and shared (the mux registers sit in the same block
that steers a dozen other pins); it *wants* to go through a privileged, checked path. Toggling
a CS is frequent, narrow, and private to one driver; it *cannot* afford one. The rest of this
chapter is the consequence of taking that split seriously.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (the user/kernel boundary and the
cost of crossing it) and ch.5 (device drivers, memory-mapped I/O).*

## Why the reflex answer -- "put it behind a syscall" -- is a trap for the hot pin

The microkernel reflex is clean and, for most things, correct: a driver is unprivileged, so
whenever it must do something the hardware only lets privileged code do, it asks the kernel
through a syscall, and the kernel validates the request before performing it. Apply that to a
pin and you get a `gpio_write(pin, level)` syscall: the driver names a pin, the kernel checks
the driver is allowed that pin, then does the store.

Now cost it. A syscall is not a function call; it is a trap across the privilege boundary. On
a Cortex-M core, `gpio_write` is: the SVC exception entry (the core stacks registers and
switches mode), instruction decode and dispatch, taking the kernel's interrupt-off critical
section, resolving the caller's handle to check the pin is really theirs, the *one store* that
is the actual work, then the exception return. The single useful instruction is buried in a
hundred-plus cycles of machinery, and **none of that machinery is removable** -- the handle
resolve *is* the check that justifies letting an unprivileged thread affect a pin at all
(Chapter 8.1). At a typical MCU clock this is on the order of a microsecond per toggle.

Put that next to the budget of the thing it is inside. A driver-owned CS brackets a transfer:
assert, clock the bytes, release. Take a modest SPI at 72 MHz:

- a **16-bit** frame is 16 / 72 MHz = **222 ns**;
- a **16-byte** transfer is 128 / 72 MHz = **1.78 us**.

The two edges of the CS are *serialized into the transaction* -- the assert must retire before
the first clock, and the release gates the next transaction. Two syscalls at ~1 us each add
**1.4-3.4 us** of pure overhead around a payload that, for the short frame, is **222 ns**. The
overhead is not a tax on the transfer; it is several times the entire transfer. And it does
this on the highest-rate path in the system, where it also drags the kernel's interrupt-off
critical section into every CS edge, injecting scheduling jitter fleet-wide. The syscall-per-
toggle answer is not merely slow here -- it is categorically the wrong shape for a hot pin.

## The reframing: the granted window already *is* the authority

The way out is to notice that the expensive part of the syscall -- the check -- does not have
to be paid *per toggle*. It can be paid **once**, and the result made into a standing fact the
hardware itself enforces.

KickOS already grants a driver its device's registers as an MMIO region the MPU maps into that
one thread and no other (Chapter 3.7). That grant is a *capability*: possessing it is exactly
the authority to touch those addresses, and the MPU is the thing that checks -- on every access,
in hardware, at zero software cost. A pin's set/clear register is just one more address. So the
move is to fold the pin's toggle register into the driver's grant. Then:

- **Allocation / permission** -- "is this driver allowed this pin?" -- is decided **once**, cold,
  at bring-up, when the window is granted (and arbitrated so two drivers never claim the same
  pin).
- **Operation** -- assert, release, read -- is a **direct store or load** to a mapped address.
  No trap, no kernel, no check *in software*, because the MPU has already drawn the boundary and
  re-checks it for free on every access.

This is the general capability lesson (Chapter 8.1) turned to its performance edge: **the
capability is the fast path.** A capability is not a ticket you show the kernel each time you
want in; it is the authority itself, and once you hold it the fast operation is *using* it
directly. A syscall wrapped around a resource you already hold a capability to is pure overhead
-- it re-litigates a question the grant already settled. The kernel's job was to decide *whether*
the driver gets the pin, not to chaperone each toggle.

## Where the syscall is still exactly right

None of this retires the checked path; it bounds it. A pin that is toggled rarely and is *not*
coupled to a bus transaction -- an LED, a module reset, a power-enable, a button read, a status
strobe -- is perfectly happy going through a kernel call. The cost that is ruinous at hundreds
of kHz is invisible at a few Hz, and routing a cold pin through the kernel keeps its handling
uniform and its authority centrally checked. The dividing line is not "how fast on average" but
**"is a toggle inside a transaction's timing budget?"** A CS edge that gates the next SPI frame
is; an LED blink never is. So the honest rule is a rate-and-coupling rule: kernel-mediated for
cold, low-rate, transaction-*independent* pins; direct MMIO for anything hot or bracketed inside
a transfer. Two tools, chosen by the pin's job, not one tool forced onto both.

## The catch, again: you can only grant what the silicon can isolate

Handing the driver its pin's toggle register *directly* re-opens the exact question Chapter 3.7
asked about whole peripherals, now at pin granularity: can the hardware hand out *just* that
authority and nothing more? Two properties pull apart, and keeping them distinct is the whole
game:

- **Allocation exclusivity is chip-independent.** "This pin belongs to that driver, and no one
  else may claim it" is bookkeeping the OS does in software. It holds on every chip, always,
  regardless of how the registers are laid out.
- **Register-grant exclusivity is a chip-dependent floor.** "The window I map for the toggle
  contains the toggle register *and nothing dangerous*" depends entirely on the silicon's
  register layout and the protection unit's granularity. This is where chips diverge.

The dangerous neighbour is the pin-mux. If a pin's set/clear register sits close enough to its
mux register that the smallest window the MPU can express covers *both*, then a grant meant only
to let the driver toggle its CS also lets it *re-route* pins -- an escalation. Whether that
happens is pure address arithmetic against the protection unit's minimum region:

- Some chips place an **atomic set/clear register with its own address**, far from the mux, and
  a protection unit with fine granularity carves a window over exactly that -- a true one-purpose
  grant. (A RISC-V PMP over a small write-1-to-set / write-1-to-clear pair is the clean case.)
- Some chips pack the output register and the mux registers **inside one minimum-size region**,
  so no window separates them -- the grant that gives you the toggle unavoidably gives you the
  remux. No kernel cleverness recovers the separation; it is arithmetic, not policy.
- Some chips have **no per-pin peripheral gate at all** (the toggle registers are reachable by
  any unprivileged code regardless), so the grant is bookkeeping only and isolation rests on
  trust.

The tempting response to the awkward chips is to force *everyone* back through a kernel syscall
so the escalation cannot happen. That trades the one thing you cannot afford -- the hot-path
latency, on every chip -- to paper over one chip's layout. The right response is the same one
Chapter 3.7 reached for whole peripherals: **state the ceiling per chip and accept it.** Where
the silicon cannot isolate a pin's toggle from its mux, the honest grant is a *dedicated* block
(arrange the board so the over-covered pins belong to that one driver anyway, and the extra
authority has no victim) or a documented trusted over-grant. You degrade one chip's isolation
story to its hardware floor; you do not degrade every chip's performance to a syscall.

## The split, made concrete

Put the two halves back together and the design is symmetric with the two jobs a pin has:

- **Configure (pin-mux): one-shot, privileged, kernel-owned.** It happens once at bring-up, it
  touches registers shared across many pins, and it is exactly the dangerous authority you do
  *not* want in a driver's hands. It is a privileged step the init path performs before it spawns
  the driver -- and it is verified, not re-performed, on any later cold claim. A driver never
  re-muxes at runtime.
- **Toggle: direct, hot, driver-owned.** The pin's set/clear register rides the driver's granted
  MMIO window; asserting and releasing a CS is a plain store the MPU authorises for free. No trap
  crosses the transaction's timing budget.

Configuration is rare, shared, and dangerous, so it pays the syscall gladly. Toggling is
frequent, private, and cheap, so it must not pay it at all. The same reasoning tells you *which*
mechanism each pin's *each job* wants -- and it is the capability model, not a special "fast GPIO
tier", that delivers the hot half: the grant the driver already holds is the fast path.

## The transferable rule

When a privileged resource is touched *rarely*, put it behind a syscall and let the kernel check
each use -- the check is cheap relative to how seldom it runs, and centralising it is worth it.
When the *same class* of resource is touched *inside a hot loop*, do not wrap the hot operation
in a syscall; instead make the authority a capability the holder already possesses -- a granted,
hardware-enforced window -- and let the fast operation *use* it directly. The kernel decides
*whether*; the hardware enforces *while*; the syscall disappears from the hot path. What you can
grant this way is bounded by what the silicon can isolate, so state that ceiling per chip rather
than pretending a portable guarantee -- and never drag the whole fleet down to the slow path to
hide one chip's inability to draw a fine line.

## Where to go next

- The peripheral-MMIO grant and the per-chip isolation ceiling this chapter extends to pins:
  Chapter 3.7, *Peripheral isolation and the hardware ceiling*.
- Why a granted capability *is* authority (not a ticket re-checked each use), and the resolve
  chokepoint a syscall pays: Chapter 8.1, *Naming a kernel object: the handle and the resolve
  chokepoint*.
- The MPU that enforces the window for free on every access: Chapter 7, *Memory protection*.
- The exact grant contract and per-chip memory-domain matrix: `../reference/architecture.md`
  ("Memory domains").
- Further reading: Tanenbaum, *Modern Operating Systems*, ch.1 (the cost of a mode switch) and
  ch.5 (memory-mapped device I/O).
