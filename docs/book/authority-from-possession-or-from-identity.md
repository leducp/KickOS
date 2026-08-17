<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Authority comes from possession, or from identity: capabilities and access lists

> Every protection system answers one question: *this subject is asking to do this thing to
> this object -- is it allowed?* There are two ways to answer it, they are transposes of the
> same truth, and which one you pick decides things that look unrelated to protection --
> how fast your IPC is, whether you can take authority back, and whether recycling a task
> slot can silently hand away permissions. Prereq: chapter 0.3 (what a kernel is for).
> Full theory: Tanenbaum, *Modern Operating Systems*, ch.9 (Security -- protection domains,
> access control lists, capabilities).

## One question, one matrix, two ways to store it

Write every subject in your system down the side and every object across the top, and put in
each cell what that subject may do to that object. That is the **access matrix**, and it is
the whole of protection. Everything else is a storage decision.

```
              sem_A      endpoint_B   uart_window
   task_1     wait,post  --           --
   task_2     wait       send         --
   driver     --         recv         read,write
```

The matrix is almost entirely empty in any real system, so nobody stores it as a matrix. You
store it by row, or by column.

**By column** is an **access control list**. Each object carries the list of subjects that may
touch it, and what each may do. To decide a request you take the subject's *identity*, find it
in the object's list, and read off the rights. This is what file permissions are, what a
firewall rule is, and what almost every configuration-driven system does.

**By row** is a **capability system**. Each subject carries the set of things it may touch, and
each entry in that set *is* the authority -- an unforgeable reference that both names an object
and conveys rights over it. To decide a request you do not ask who the subject is. You check
that the reference it presented is one it actually holds.

Same matrix. Opposite storage. And from that one choice, everything below follows.

## The runtime difference: a lookup versus a possession test

Under an access list the check is: *authenticate the subject, then search.* The subject hands
you a **name** -- a path, an id, an integer -- and you consult a table keyed on who is asking.

Under capabilities the check is: *is this reference real, and does it carry the right?* The
subject hands you the authority itself. There is no table keyed on identity, because identity
is not what authorises anything.

That difference has a cost consequence which decides the matter for a microkernel, and it is
worth stating before the philosophy. **In a microkernel, IPC is the hot path.** Files, network
stacks, drivers -- everything that would be a kernel call elsewhere is a message to a
userspace server here, so the authority check on the message path runs at the frequency of
*all system activity combined*. A possession test is a bounds check, a generation compare and
a rights mask on a reference the caller already holds. An identity-keyed lookup is a search,
on every send. One of those you can afford in the hottest path in the system; the other you
cannot.

This is not a small-system argument. It is why the whole L4 lineage, which exists because IPC
speed is the thing that decides whether a microkernel is viable at all, is capability-based.

## Where they diverge

### Ambient authority, and the confused deputy

Under an access list, every subject can *name* every object. Naming is unrestricted; the check
is the only thing standing between the subject and the object. Authority is **ambient** -- it
surrounds you, and you invoke it by mentioning a name.

That produces a failure mode with a name of its own. A service acting on behalf of a client
uses *its own* identity when it touches things, because identity is what authorises. So a
client can ask the service to touch something the client could not touch itself, and the
service -- holding more authority than its caller -- does it. The service has been made into a
**confused deputy**: it did exactly what it was asked, with authority that was never the
asker's to use. (Norm Hardy named this in 1988, describing a compiler that would happily
overwrite the billing database because the *compiler* was allowed to.)

Under capabilities the client must hand the reference over. The deputy cannot reach what it
was not given, because it cannot *name* what it was not given. The failure mode is not
mitigated; it is unrepresentable.

### Delegation

Passing authority is the native operation of a capability system: give someone the reference.
It is transitive without the kernel needing a policy, and it composes -- a parent handing a
child an enumerated set of references is a complete, auditable statement of that child's
authority, written at the moment of creation.

Under an access list, giving authority means adding an entry to a list, which means the
subject must be *nameable in advance*, and something privileged must do the adding. Transient,
fine-grained delegation between peers is awkward at best.

### Enumerability, in both directions

The two models make opposite questions cheap, and this is the clearest illustration that they
are transposes:

- *"What can this subject reach?"* -- one read of its row under capabilities. Under an access
  list, a scan of every object in the system.
- *"Who can reach this object?"* -- one read of its column under an access list. Under
  capabilities, a scan of every subject.

The first question is the one a *least-authority* argument needs: to say "this driver can touch
exactly these three things" you must be able to enumerate a subject's authority. The second is
the one an *audit* needs. Neither model is better; they make different questions cheap, and you
should know which question you will be asked.

### Revocation: the access list's real win

Take authority back under an access list and you are done: remove the entry. The check happens
at access time, so the next attempt fails. Cost, O(1), one place.

Take authority back under capabilities and you have a problem, because a capability is
authority *already distributed*. Somebody holds it. To revoke it you must find every copy --
which means the system has to have been remembering, all along, who derived what from whom.
That is a real data structure with a real price: seL4 keeps a capability derivation tree, and
pays for it with half of every slot in every capability table, present whether or not the
capability is ever derived. Other designs pay differently: some make revocation all-or-nothing
(destroy the object identity and every reference to it everywhere dies), and some decline
revocation entirely and let a holder keep what it was given until it exits.

**So if selective revocation is a requirement, an access list gives it away free and a
capability system makes you buy it.** That is the honest trade, and it is the strongest thing
that can be said for the access-list side.

### The namespace

An access list needs a global namespace: every subject must be able to *name* every object in
order to ask about it. That namespace is itself a surface. If "you may not" and "there is no
such thing" are distinguishable answers, the namespace is an oracle for probing what exists.
Capability systems need no such namespace -- the set of names a subject can form is exactly
the set of things it holds.

## Rights belong to the pair, not to the name

A subtlety that catches people, and it is the same in both models once you see it: **rights are
an attribute of the (subject, object) pair.** They are not a property of the object -- two
subjects can hold the same object with different rights -- and in a well-built capability
system they are not a property of the *token* either.

That last point is worth dwelling on, because it is what makes a capability safe to hand to
untrusted code. The thing userspace holds should be an opaque name -- an index, a number -- and
the rights should live where the *kernel* keeps them. If rights were encoded in the token, the
holder could inspect them, and every bit of the encoding would be an invitation to try
forging a better one. When the token is just a name, "widen my own rights" is not a badly
guarded operation; it is not an operation at all.

A consequence that surprises people: the same subject may legitimately hold the *same object
twice*, at two names, with different rights. A server may hold an endpoint with full rights
while also holding a send-only reference to it that it hands to clients. That is not a
redundancy to optimise away -- it is the mechanism by which a narrower authority is created
without touching the wider one.

## The third thing, which fits neither model

Some permissions name no object at all. "May allocate memory." "May change the clock." "May
configure a pin." There is no object to put in a matrix cell, so neither a row nor a column has
anywhere to put them.

Trying to force them in produces one of two messes: a fake object that exists only to be named
(and then needs its own lifecycle), or a magic reserved index that is a permission bit wearing
a reference's clothing. The clean answer is to stop pretending, and keep a small set of
**authority bits** on the subject itself, separate from its capability set. It is a genuinely
different kind of permission and it deserves a genuinely different mechanism.

## Three questions that actually decide it

Skip the philosophy and ask these:

1. **Is an authority check on your hot path?** If yes -- and in a microkernel it is, because
   IPC carries everything -- a possession test wins and the argument is over.
2. **Do you need to take authority back from a running subject?** If yes, an access list gives
   it to you free and a capability system charges you for it. Decide *before* you build, not
   after; retrofitting revocation into a capability system means retrofitting a derivation
   structure into every table.
3. **Is your subject identity stable, or do you recycle it?** This one is the trap, and it is
   the next section.

## The trap when authority is keyed on identity

An access list is keyed on *who you are*. So it depends, silently and completely, on subject
identity meaning one thing for as long as any permission mentioning it survives.

Now recycle a subject slot -- which every embedded system does, because a thread pool is a
fixed array and a dead thread's slot is reused. If permissions are keyed on the slot **index**,
the index outlives its occupant, and the new occupant inherits whatever the old one was
granted. The permission was not copied and nothing was corrupted: the identity simply changed
meaning underneath a record that mentioned it.

This is not hypothetical, and it is not a bug you can review your way out of once. A
widely-used RTOS carried an index-keyed permission bitmap for roughly eight years in which
allocating a thread index without holding the right lock could hand two threads the *same*
index, aliasing one permission bit across both -- so granting access to one silently granted it
to the other (CVE-2026-10681). The defence in that design is to scrub the recycled index out of
every object's bitmap before reissuing it, which is work proportional to the number of objects,
on every thread creation.

A capability system meets the same hazard in a different place -- a stale *reference* to a
recycled slot -- and answers it with a generation counter, which turns the aliasing into a
detectable failure instead of a silent grant. That is chapter 8.7's subject, including the part
everyone skips: what the width buys you is a probability, not a guarantee, and the allocator's
choice of which free slot to hand back is part of the guard.

## What the field chose

Presented for the option space, not as a ranking -- each of these buys something and pays for
something, and the choices track what each system is *for*.

| Approach | Authority from | Rights in the reference | Revocation |
| --- | --- | --- | --- |
| seL4 | possession | fused into the capability word | transitive, via a derivation tree |
| Zircon (Fuchsia) | possession | fused, immutable once minted | none; reference counting only |
| Genode | possession | **none at all** | destroy the object identity, globally |
| Zephyr | identity, per-object bitmap | one bit -- access or none | per subject, by clearing a bit |
| PikeOS | identity, static configuration | no; validated at open | not at runtime, by design |
| INTEGRITY-178B | identity, static access matrix | no; kernel-side tables | halt and reconfigure |
| QNX Neutrino | identity, plus ambient ability bits | no; cached server-side at open | per channel or per path |

Two entries repay a second look. **Genode carries no rights bits at all**, on the reasoning that
a narrower right should be a capability to a narrower *object* rather than a mask on a shared
one -- a coherent position, and the reason to weigh it is that it costs an object per
distinction. And the two safety-certified separation kernels decided their matrices at
integration time and forbid runtime change: with no delegation and no revocation, worst-case
memory and timing become computable, which for a certified system is worth more than the
flexibility it gives up.

## How KickOS answers it

Possession, for the reason in question 1: IPC is the hot path and everything is built on it.
Resolution is a bounds check, a generation compare and a rights mask -- see chapter 8.1 for the
resolve chokepoint and 8.7 for the generation.

The pieces map onto this chapter as follows. A capability entry fuses the **reference** (which
object) with the **rights** over it and a **generation** that detects staleness. What userspace
holds is none of those -- it is an opaque task-relative name, so rights are unforgeable by
construction. Permissions that name no object live in a separate authority word on the thread,
per the section above. There is no revocation, which is question 2 answered deliberately rather
than by omission: the derivation structure that would provide it costs more, on a 16 KiB part,
than the property is worth here.

The exact contract -- entry layout, rights bits, the reserved index convention, the authority
bits -- is `../reference/architecture.md` ("Object model, capabilities & IPC") and
`../reference/invariants.md`. This chapter explains why the shape is what it is; the Reference
says what it is, and wins on any disagreement with the code.
