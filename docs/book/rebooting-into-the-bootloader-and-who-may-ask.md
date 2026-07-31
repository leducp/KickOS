<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Rebooting into the bootloader, and who may ask

> Three different acts hide behind the word *reboot*, and telling them apart is most of
> the work. Stopping execution, restarting your own firmware, and ceasing to be the
> firmware so that something else may install a new one differ in what survives, in what
> the host must do next, and above all in blast radius -- and only the third makes an
> unattended development loop possible. The mechanism for that third act is unportable
> in a way no seam can paper over, so the seam has to be allowed to answer "this chip
> cannot"; where a vendor does hand you an indirection, the stable thing is the *name*
> and never the address; and deciding who may ask is a sharper capability question than
> it looks, because bootloader entry is keyed to no resource at all and therefore cannot
> be authorised by possessing one. It closes on the one claim nothing inside the target
> can witness, and on the way out of that: when the destroyed observer leaves behind a
> machine that admits the next operation, the absent button press *is* the measurement --
> provided the hand-back is wired at every place the system can end, which is more than
> one place. Binds to [`../reference/architecture.md`](../reference/architecture.md)
> ("User/kernel separation", "Object model, capabilities & IPC"),
> [`../reference/invariants.md`](../reference/invariants.md), and
> [`../reference/porting.md`](../reference/porting.md) (the arch-seam fallback
> convention).

## The problem: "reboot" names at least three acts

A developer asks for "reboot" and means one of three things. They are not variants of
each other; they are different operations with different survivors, and a design that
offers one when the caller wanted another is not a small inconvenience.

**Stop executing.** The system ends. Threads stop being scheduled, the core is parked or
halted, nothing further happens until a human or a debugger intervenes. Nothing about
the machine's configuration is undone: clocks stay where they were retuned to,
peripherals stay ungated, RAM keeps its contents until power goes. What survives is the
whole machine state, minus anybody to look at it.

**Restart my own firmware.** The core is reset and the same image runs again from its
reset vector. This is the one most people picture, and it is the one with the subtlest
survivors: a *core* reset and a *system* reset are different reaches on most parts, so
whether a peripheral, a clock tree, a retention register or an external device is
reinitialised depends on which reset you asked for. A system that recovers by resetting
itself has to know exactly which of its assumptions the reset actually re-established,
because the ones it does not re-establish are now stale rather than absent.

**Stop being the firmware.** The chip re-enters the code that runs *before* an
application exists -- a boot ROM, a resident bootloader, or a companion chip -- in the
mode where it will accept a new image over some transport. Your firmware does not run
again. What survives is only whatever the ROM path leaves alone, and what the host must
do next is not "wait for the banner" but "notice a different device and program it".

Laid side by side, the difference that matters is not mechanism but consequence:

| Act | Runs next | Host's next move | Blast radius |
|---|---|---|---|
| Stop executing | nothing | attach a debugger, or power-cycle | availability: the board is idle until someone acts |
| Restart my firmware | the same image | wait for it to come back | availability, plus whatever the reset did not re-establish |
| Enter firmware download | the ROM or bootloader | send a new image | the board will accept **arbitrary** firmware |

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (what the ROM firmware does
before an operating system exists, and why the boot path is not part of the OS it
starts) and ch.9 (protection domains and the principle of least authority -- the frame
for the second half of this chapter).*

## Only one of the three is a development affordance

The reason to care about the third act is narrow and practical: it is the only one that
removes the human from the flash loop.

A development cycle on a real board is build, flash, run, read. The flash step usually
requires the board to be in a mode it does not boot into, entered by holding a button,
strapping a pin, or power-cycling with a pin held. As long as that is true, every
iteration has a hand in it -- which forbids soak tests that reflash between runs, a
board in a rack, or a bench that is not in the same room as the developer. A firmware
that can put *itself* into download mode turns the loop into a program.

The first two acts do not buy that. Stopping execution leaves the board in a state the
flashing tool still cannot talk to. Restarting the same image runs the same image again,
which is precisely what you were trying to replace. Only "stop being the firmware" hands
the transport to something that will take a new one.

That narrowness is the whole justification, and it is worth holding on to, because it is
also the answer to several questions later in the chapter: this is a *bench* facility.
It is not a recovery mechanism, not a watchdog action, not a system service an
application would call in the field.

## Why the mechanism cannot be portable

Now the unwelcome part. There is no architectural instruction for "enter the
bootloader", because the bootloader is not an architectural concept. A core reset often
*is* architectural or nearly so -- a control register, a write with a key -- but the
thing that decides what runs after a reset is the vendor's boot design, and vendors have
chosen genuinely different structures. Four shapes cover most of the field, and it is
worth learning them as shapes rather than as products, because a new part will be one of
them.

**A ROM function table reached by a name-like code.** The boot ROM stays resident and
publishes its own entry points through a lookup table. Nothing is at a documented
address; instead a *code* is looked up at run time. The Raspberry Pi RP-series bootrom is
the clean worked example. A fixed spot near the very start of the ROM carries a magic
marker -- two ASCII characters and a byte naming the table *layout* -- and immediately
after it sit halfword pointers to the function table, the data table, and a lookup
helper. Code that wants an entry point calls the helper with a two-character code:
`UB` asks for the reset-into-USB-bootloader routine, `RB` for the general reboot routine
whose flags select bootloader entry among other targets. The pointers are halfwords
because the ROM is small and lives at the bottom of the address space, which is a nice
reminder that this is a table in a real chip and not an abstraction.

**A companion processor that owns your debug port.** Some boards put a second, small
microcontroller alongside the application chip whose only job is to speak USB to the
host and to program the application chip through its debug interface. The PJRC Teensy 4
line is the worked example: the application core exposes no debug header of its own, and
the bootloader chip drives the debug port internally. On such a board, "enter the
bootloader" is not a call at all. It is *making yourself observable in a way the
companion chip is watching for* -- on that part, executing a breakpoint instruction with
no debugger attached, which the companion notices and answers by taking the chip into
programming mode. The lesson generalises beyond the trick: when another agent owns the
transition, your firmware's job is to produce a signal, and the semantics belong to a
device you do not control and cannot test in isolation.

This shape also carries a different *kind* of claim behind it than the others, and the two
are worth keeping apart. A published lookup table is a contract: the vendor names the
code, the code resolves, and a caller may rely on it on parts that do not exist yet. A
signal noticed by somebody else's firmware is a behaviour -- learned from what a companion
has been seen to do, and standing on nothing the vendor undertakes to preserve. Nothing
forbids depending on it, but it is an observation about one companion's firmware rather
than a promise about the part, and the only thing that can establish it is the board.
Reading the four shapes includes reading the strength of what stands behind each: a
documented indirection and an inferred behaviour are not the same class of claim, even
when both work.

**A ROM API that enters a vendor download protocol.** Larger application-class MCUs
often ship a boot ROM with a documented API tree, one of whose entry points re-enters the
ROM in its *serial downloader* mode -- the mode the vendor's own production programming
tools speak. NXP's i.MX RT family is the worked example. This shape is the interesting
one because it *works* and may still be useless: entering the vendor's download protocol
is only an affordance if the transport and the host tool on the other side are the ones
your workflow actually uses. A board whose established flash path is a third-party
bootloader over USB gains nothing from being dropped into a protocol that path does not
speak; you have successfully stopped being the firmware and arrived somewhere nobody is
listening.

**No mechanism at all.** Many parts simply have no bootloader to return to. The image in
flash is the only code on the chip; entry into programming mode is a physical act on a
strap pin or a debug probe. There is nothing to call, and nothing clever to do instead.

Read those four together and the conclusion is forced: bootloader entry has no portable
mechanism, not because nobody has abstracted it yet, but because the thing being
abstracted is a per-vendor boot architecture. Which is a specific instance of the rule
that governs every hardware-facing claim -- see
[*Peripheral isolation and the hardware ceiling*](peripheral-isolation-and-the-hardware-ceiling.md)
for the general form: an OS cannot offer a capability the silicon does not have, and
writing the offer down anyway does not create it.

## A seam that is allowed to say no

So the seam over this has to be able to decline, and declining has to be an ordinary,
documented answer rather than an embarrassment.

The habit that gets this right is the one the arch layer already uses for facilities that
exist on some parts and not others: a **fallback that returns "not implemented"**, alone
in a translation unit of its own, which a chip may displace and most will not. A chip that
defines the symbol satisfies the reference from its own archive member, so the fallback
member is never extracted -- linker behaviour rather than a language rule, but behaviour
every Unix-like linker and MSVC `.lib` share, unlike a weak attribute whose interaction
with archives is implementation-defined. A pin-mux seam is the familiar case -- parts with
a central mux block define it, parts whose pin function lives per-peripheral keep the
declining fallback, and no fake backend is written to make the fleet look uniform
(`../reference/porting.md` carries the exact convention). Bootloader entry belongs in
exactly that family, and more urgently than most, because three of the four shapes above
are unimplementable on a part that is not that shape.

What makes this more than a style preference is what the alternatives do to a caller.

A **stub that returns success** and does nothing is the worst possible answer here. The
caller of a bootloader-entry request does not expect a return value; it expects to stop
existing. A host script that issued the request will now wait for a device that will
never appear, and the failure surfaces as a timeout in the flashing tool -- somewhere
far from the chip that lied. Compare the honest refusal: the caller gets a negative
errno, on the spot, from the syscall it just made, and a bench script can branch on it
and fall back to asking a human for the button.

A **stub that pretends to be a reset** -- reaching for the architectural core reset
because it is available and looks close enough -- is worse still, in a quieter way. It
succeeds at *something*, so nothing reports an error, and the board comes back running
the very image you were trying to replace. Now the loop appears to work and silently
tests a stale binary. Substituting a different act because it is the one you can perform
is how a portable seam becomes a lie that no test catches.

Two other honest answers exist and are worth naming. A part where entry is a physical
act can decline and let the workflow keep the button press, which is a real answer rather
than a gap. And a part that can only enter a download protocol nobody in the workflow
speaks can also decline, on the grounds that landing in an unreachable mode is a worse
outcome for the caller than a refusal. The seam's contract is "put this board into a
state a host can reflash, or tell me you cannot" -- not "execute the closest available
instruction".

## The stable thing is the name, not the address

The ROM-table shape carries a lesson about vendor ABIs that is worth extracting on its
own, because the mistake it invites is easy, feels defensive, and is wrong.

A lookup table exists **because the addresses behind it move.** The vendor guarantees
that the code `UB` will find the reset-into-bootloader routine on every revision of the
part; it guarantees nothing at all about where that routine sits. Silicon revisions add
routines, reorder them, and change their sizes. Absorbing exactly that churn is the
table's entire function -- it is a symbol table in ROM, and the two-character code is a
symbol.

Which makes the tempting defensive move a reimplementation of the mechanism it distrusts.
The move looks like this: read the ROM's build-version byte, branch on it, and use the
address that version is known to have. Every part of that is backwards. It converts a
guarantee the vendor made (the name resolves) into an assumption the vendor never made
(this build has that layout). It requires a new arm for every future revision, so it
fails on silicon that did not exist when it was written -- the exact failure the table
prevents. And it is more code than calling the helper.

There is a real distinction hiding in "version", and it is the reason the mistake sounds
plausible:

- A **layout version** is part of the magic marker. It says which *shape* of header you
  are looking at, and you are about to trust that shape -- to read pointers at fixed
  offsets from it and call through them. Checking it is not defensive clutter; it is
  validating a structure before dereferencing it, and refusing to proceed when the marker
  is unrecognised is the correct behaviour. When one ROM serves two instruction sets, the
  layout is what tells you the table's entries carry per-architecture flags and that the
  lookup helper wants to be told which entry point you mean.
- A **build version** is informational. It identifies the ROM revision for the benefit of
  a human reading a bug report, or for code that must work around one specific documented
  erratum in one specific build. It is not a feature-detection channel, and vendor
  documentation generally says so in as many words.

The general rule, for any ROM or firmware ABI reached by table: **check the layout, then
use the names.** If you find yourself branching on a build number to locate something, the
question to ask is not "have I covered every build" but "what indirection was I given, and
why am I not using it".

## Who may ask: shutdown and bootloader entry are not the same cost

Now the authority question. Superficially it is settled before it is asked: bootloader
entry ends the running system, ending the running system is what shutdown does, so
bootloader entry goes wherever shutdown went. In a system with a small fixed budget of
named authorities -- the device-class authority that already covers ending the system and
taking over the console, say -- the fusion is nearly free and needs no new machinery.

Materially the two acts are not the same cost, and the difference is a category and not a
degree.

**Shutdown costs availability.** The system stops. Whatever was running is gone, and
somebody has to intervene. That is a denial of service against the board, and against the
board only; the image in flash is untouched and the next power cycle runs it.

**Bootloader entry costs integrity, persistently.** A board sitting in firmware-download
mode will accept *arbitrary* firmware from whoever reaches the transport, and the
firmware it accepts is the firmware it runs after the next reset, and the one after that.
Anything that can put the board into that mode can therefore replace everything the board
is -- not stop it, replace it. Recovery is not a power cycle; recovery is reflashing from
a trusted host, if you still have one.

So fusing them means that anything permitted to end the system, or to publish a console,
is also permitted to hand the board to an attacker permanently. That is the honest
statement of the cost, and it should be written down wherever the fusion is made rather
than discovered later by someone reading the rights bits.

There are real reasons a design may still fuse them.

**A rights-bit budget is finite, and spending is irreversible-ish.** When authorities live
as bits in a fixed-size rights field, the last free bit is worth more than the
distinction it would buy, because the *next* authority after it has no home at all --
adding one means merging two existing authorities, or growing a frozen structure. Naming a
new authority is cheap only until it is not, and a design gets exactly one chance to spend
the last bit well. (What that budget is and why it is fixed is
[*Privilege is three axes, not one bit*](privilege-is-three-axes-not-one-bit.md).)

**A feature absent from the image carries a different risk calculus.** An act compiled out
of production builds cannot be reached by any caller, authorised or not, which is a
stronger guarantee than any run-time check -- absence beats refusal, because absence has
no bug in it. If bootloader entry exists only in bench images, the population that can
reach the fused authority is the population you already trust with the bench.

And the counter-argument has a cost worth stating plainly, so the trade is visible in both
directions. Fusion couples the two acts *for as long as the fusion lasts*, and the build
switch is the only thing separating them. The day the facility is wanted in an image that
ships, the authority model has to be split first -- and splitting it later is more
expensive than splitting it now, because by then the fused authority has other holders who
were granted it for the other reasons it covers. A recorded counter-argument with a named
escape route (which two authorities to merge to free a bit) is not the same thing as
having gotten the answer right; it is the minimum needed for the decision to be revisitable
by someone who was not there.

## Scoped authority and systemic authority

This is the idea worth taking away from the authority half of the chapter, and reboot is
only the example that makes it obvious.

Privileged acts in a capability system fall into two kinds, and the kind decides *where
the authorisation can live*.

**A scoped act is keyed to a resource.** Configuring a peripheral you were granted;
toggling a pin in a register window that is yours; claiming an interrupt line; sending on
an endpoint. Each of these names a thing, and the act's entire effect lands inside that
thing. For a scoped act, **possession can itself be the authorisation** -- if you hold the
capability for the resource, and the effect cannot escape the resource, then the check has
already happened at grant time and re-asking is redundant. That is exactly why a granted
MMIO window *is* the authority to drive the device inside it, with no syscall on the hot
path (Chapter 8.4,
[*The fast path is the capability*](the-fast-path-is-the-capability-gpio-direct-mmio.md)).
Scoped acts also narrow gracefully: a smaller window, a single line, one endpoint. There is
an axis along which to give less.

**A systemic act is keyed to nothing.** Ending the system, retuning the core clock that
retimes every deadline in it, entering firmware-download mode. Ask what resource such an
act is scoped to and the answer is "the machine", which is another way of saying the
question does not apply. Two consequences follow, and neither is a matter of taste:

1. **Possession of anything cannot authorise it.** There is no holding whose scope
   contains the effect, so no grant can imply it. A system that lets some resource
   capability imply a systemic act has made every holder of that resource a holder of the
   machine -- which is the confused-deputy shape wearing capability clothes.
2. **It must take an explicit authority.** Since the act cannot be authorised by
   possession, it has to be authorised by *name*: a right the kernel checks at the gate
   and can refuse. And because there is no resource to narrow, the only dimensions such an
   authority has are holding it, not holding it, and having it taken away -- which is why
   making systemic authority *droppable data* rather than an identity bit is where the
   value is (Chapter 7.4 again).

Bootloader entry is the cleanest systemic act there is, which is what makes it a good
teaching example rather than a special case. Its effect is not merely system-wide, it is
*system-replacing*. No window, no line, no object stands in the right relationship to it.
Notice that this reasoning is independent of the rights-budget argument in the previous
section: *that* it needs a named authority is settled by scope, and only *which* named
authority it gets is a budget question. Conflating the two is how systemic acts end up
riding on a resource grant because the resource grant happened to be nearby.

The general rule for placing any act in a capability system is therefore one question,
asked before the plumbing: **what resource is this act keyed to?** If the answer names
one, the grant for that resource can carry the act, and the act should narrow with it. If
the answer is "the machine", the act needs a name of its own, and no amount of holding
anything else may substitute for it.

## The witness problem: an act that destroys its observer

One problem is left, and it is a testing one -- genuinely awkward at first sight: **a
successful bootloader entry cannot be witnessed from inside the target.**

An in-target test asserts by running code after the thing it tested. Here there is no
after. The code that would evaluate the assertion, the console that would print it, and
the harness that would count it are all gone -- replaced by a ROM that has never heard of
any of them. There is no return value to check, because the call does not return; a call
that *does* return has, by definition, failed. You cannot even log the attempt reliably,
since the transition may complete before a buffered console has drained.

What *is* fully testable is every arm that survives, and there are more of them than the
untestable one:

- **The unauthorised caller.** A thread lacking the required authority gets a refusal, and
  the refusal is an ordinary syscall return in a system that is still running. This arm
  is testable on every target, including emulated ones and a host simulator, because it is
  pure kernel logic that never touches the chip. It is also the arm most worth testing,
  since it is the security property.
- **The unsupported chip.** A part whose seam declines returns the not-implemented code,
  and the caller is still alive to read it. Also fully testable, also on any target.
- **The refusal that must be checked to fail.** A gate proving a guard *exists* is worth
  much less than one that has been observed to fire when the guard is removed. That
  discipline applies with full force here, precisely because the success path can never
  contribute evidence.

The success path can be witnessed only from the **host**, and only as a change in the
device it sees: the board stops presenting whatever interface the firmware presented and
starts presenting the bootloader's. That is real evidence, and it is *external* evidence,
which means it belongs in the record as such rather than as a test result.

**There is a sharper form of that, and it is the more useful idea.** Look at what the host
does with the changed device: it flashes the board, and it does so with no button press.
The act under test is precisely the act that makes the next act possible -- so **when a
destructive operation returns the machine to a state that admits the next operation, the
absence of the manual intervention *is* the measurement.** The next successful load is not
merely consistent with the previous hand-back having worked; it is unavailable unless it
did. Nothing has to assert anything, and nothing has to be watched. The proof and the
benefit are the same event.

That inverts the awkwardness the section opened with. An act that destroys its observer has
no observer *inside*, but it may still have a **successor** -- and a successor that is
impossible without it is a stronger witness than most assertions, because it is not a claim
about the system, it is the system continuing to work. So before concluding that a
destructive act is unmeasurable, ask what it makes possible: what step becomes available
only if this succeeded, and can that step be made part of the ordinary loop rather than of
a test? Operations shaped this way are not rare -- an update that must boot far enough to
accept the next update, an unmount whose proof is the next mount, a release whose proof is
the next acquisition, a migration whose proof is that the following one applies cleanly.
Each of them pays for its own verification.

Two conditions make such a witness real, and both are easy to lose without noticing.

- **The successor must genuinely be impossible otherwise.** If the loop keeps the button
  press as a quiet fallback, taken whenever the automatic path did not happen, then a
  successful load no longer distinguishes the two cases and the evidence is gone. A
  fallback that silently covers for the mechanism destroys the measurement it was added to
  protect. Keep it out of the measured path, or make taking it loud.
- **The failure has to stop something.** A self-proving act reports its own regression by
  the loop halting rather than by an assertion failing, which is only useful if the halt
  reaches somebody. A witness nobody is waiting on is not a witness.

Generalise the rest of the shape too, since destroying your own observer is a common shape
-- shutdown, a self-reset, disabling the clock that feeds your own core, releasing the
memory your stack is in:

- **Test the arms that survive**, and recognise that the refusal arms usually carry the
  property you actually care about. The gate is the security claim; the transition is
  plumbing.
- **Look for the successor before declaring the act unmeasurable.** A self-proving act
  needs no assertion, cannot quietly stop being exercised, and cannot be forgotten, because
  the workflow that depends on it is the thing running it.
- **Say out loud what nothing witnesses.** An honest record distinguishes "asserted by a
  test", "attested by a successor that could not have happened otherwise", "seen on
  hardware by a human watching the host", and "believed from the datasheet". Those are four
  different strengths. The temptation is to let a passing suite imply the whole feature
  works, when the suite structurally cannot reach the interesting half.
- **Know which witnesses can even hold the question.** An emulator with no model of a boot
  ROM cannot witness a successful entry no matter how the test is written -- not weakly,
  but not at all -- while it witnesses the refusal arms perfectly. This is the same
  emulator-versus-silicon reading the Reference applies to every hardware claim: a witness
  is only as good as the mechanism it models, and stating which witness saw what is more
  useful than a single pass/fail.

## A system has more than one way to die

Making the loop unattended means the firmware has to *issue* the hand-back by itself, which
means choosing where in the image the request sits. The obvious place is the end -- and the
obvious assumption is that there is one end, that all terminal paths converge somewhere, so
one hook at the convergence covers every run.

That assumption is false, and it is false structurally rather than by oversight.

**Ordered exits are one family.** A shutdown request, the last thread leaving, an explicit
panic: these run in ordinary context, on a stack that is intact, with the console still
drainable, and they arrive at a common place because somebody wrote them to. **A fault is
not in that family.** It arrives because hardware entered an exception vector, and the
handler that answers it is written to report as much of the machine as it can and then
stop -- dead-ending on purpose, because almost everything it might call is now suspect. The
two never meet. **The exits you reach on purpose and the exits you reach by accident are
different paths, and a hook on one is absent from the other.**

Which lands exactly wrong for a flash loop. The images that fault are the images you most
want to replace, and they are precisely the images an ordered-path hook never covers. The
failure is silent in the flattering direction: every run that ends cleanly hands the board
back and reflashes, so the mechanism looks complete until the first crash, and then the
board sits there presenting a firmware interface nothing can flash. A self-proving loop at
least fails honestly here -- it stalls rather than reporting a pass -- but a stall is still
the loop stopping on the runs that mattered most.

The general lesson is worth more than the flash loop. **Enumerate a system's terminal paths;
do not assume them.** The count is not how many functions are named for ending, it is how
many places can be the last thing that runs. Anything that must happen on the way out --
flushing a log, releasing a bus, dropping a chip select, parking an actuator, handing the
board back to its bootloader -- has to be attached to each of them, or explicitly declined
for the ones where it cannot run.

And "attached to each" is not the same as pasting the same code everywhere, because a fault
path is the one context where doing *less* is right. It cannot take a lock, cannot send a
message, cannot trust the stack it is standing on, and should not wait on a device. So the
question per exit is not only whether the act belongs there but whether it can survive
there. A bootloader hand-back happens to fare unusually well by that test -- a register
write or a ROM call needs no scheduler, no allocator and no working stack beyond a few
words, which makes it one of the few useful things a dying image can still do. Other exit
acts fail the test outright, and for those the honest answer is that this way of dying does
not get them.

## A development affordance, not a system service

Everything above converges on the same conclusion about what this facility *is*.

Its justification is a bench loop. Its blast radius is the whole board, persistently. Its
mechanism exists on a minority of parts and is different on each of them. Its success path
is unwitnessable from inside, and provable only by what it lets a host do next. Nothing on
that list describes a system service; all of it describes a tool.

The design consequence is to treat it as one, which means keeping it out of images where
its justification does not apply. A build switch that leaves the syscall, the seam, and the
gate out of production builds costs nothing in a bench image and removes the entire
question from a shipped one -- no reachable gate, no authority to reason about, no attack
surface to argue over. That is a stronger guarantee than any run-time check can offer,
because an act that is not in the image cannot be reached by a caller who was not supposed
to reach it, nor by one who was supposed to and had a bug.

It also puts the fused-authority trade in its proper place. The fusion is defensible for a
facility that is absent from production images, and it is only defensible *because* of
that absence. So the build switch is not a convenience wrapped around the feature; it is
load-bearing in the authority argument, and anything that would make the facility
generally available has to revisit the authority split first, not afterwards.

## The transferable rules

- **Name the act, not the word.** "Reboot" is three operations. Decide which one a caller
  needs by asking what must survive and what the host must do next, and never substitute a
  neighbouring act because it is the one the chip can perform.
- **Let a seam refuse.** A fallback that returns not-implemented is a better port than
  a stub that returns success, and far better than one that performs a different act. The
  contract of a hardware-facing seam includes the answer "this part cannot".
- **Use the names a vendor made stable; check layouts, not builds.** An indirection you
  were handed exists because the addresses behind it move. Branching on a build version to
  find a function reimplements the table badly and fails on silicon that does not exist
  yet.
- **Ask what resource an act is keyed to.** Scoped acts can be authorised by possessing
  the resource and should narrow with it. Systemic acts are keyed to nothing, so no
  possession can authorise them and each needs a name the kernel can check and revoke.
- **Test what survives, and say what nothing witnesses.** For an act that destroys its
  observer, the refusal arms carry the property worth proving, the success is host-side
  evidence, and the honest record distinguishes the two instead of averaging them.
- **Let a destructive act prove itself through its successor.** If the act leaves a machine
  that admits the next operation, the missing manual step is the measurement: the proof and
  the benefit become the same event. Guard it by keeping the fallback out of the measured
  path and by making the halt visible.
- **Count the ways the system dies.** Ordered exits and faults do not converge, so an exit
  act wired only on the deliberate path is missing from every image that ends by accident --
  the ones you most wanted it for. Enumerate the terminal paths, and for each one either
  attach the act or say why it cannot run there.

## Where to go next

- What the authority half of "privileged" is, why it lives in droppable data, and where
  the rights-bit budget comes from: Chapter 7.4,
  [*Privilege is three axes, not one bit*](privilege-is-three-axes-not-one-bit.md).
- Why an OS cannot promise what the silicon will not do, worked on peripheral isolation:
  [*Peripheral isolation and the hardware ceiling*](peripheral-isolation-and-the-hardware-ceiling.md).
- The scoped side of the scoped/systemic split, where a granted window *is* the authority:
  Chapter 8.4,
  [*The fast path is the capability*](the-fast-path-is-the-capability-gpio-direct-mmio.md).
- The gate every systemic act passes through, and why the kernel must validate before it
  acts: Chapter 3.9,
  [*The syscall path: trap, dispatch, return*](the-syscall-path-trap-dispatch-return.md).
- How an authority is named, narrowed and resolved: Chapter 8.1,
  [*Naming a kernel object*](handles-and-the-resolve-chokepoint.md).
- The exact contracts: [`../reference/architecture.md`](../reference/architecture.md)
  ("User/kernel separation", "Object model, capabilities & IPC"),
  [`../reference/invariants.md`](../reference/invariants.md), and
  [`../reference/porting.md`](../reference/porting.md) (arch seams and their fallback
  translation units).
