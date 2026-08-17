<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Alignment across the syscall boundary

A syscall is the one place unprivileged code hands the kernel a value the kernel then acts on
*with privilege*. The kernel must treat every such value as hostile until proven safe -- this is
the confused-deputy discipline (see `../reference/architecture.md`, "Syscall-argument
validation"). Ownership is the obvious axis: a user pointer must lie inside a region the caller
was actually granted, or the kernel becomes a laundering service for memory the caller could not
reach on its own. But there is a quieter axis that bites *before* ownership ever matters:
**alignment**. This chapter is about why the kernel cannot assume the alignment of a caller's
pointer, why the correct requirement is arch-specific, and how a single hardcoded number turns a
"hardening" check into a portability bug.

## Why the kernel touches a user pointer at all

Some syscalls return more than fits in a register, or take a struct. KickOS keeps the syscall ABI
register-based (`sys/abi.h`), so a 64-bit result is either split into register halves or written
through a caller-supplied **out-pointer**: the stub allocates a local, passes its address, and the
kernel stores the result there. `clock_now` is the canonical example -- the userspace stub is

```
uint64_t out = 0;
arch_syscall(KOS_SYS_CLOCK_NOW, (uintptr_t)&out, ...);
return out;
```

and the kernel, running privileged, does `*(uint64_t*)a0 = arch_clock_now()`. Likewise a
struct-taking syscall (thread spawn) copies the caller's struct into kernel memory before reading
its fields. In both cases the kernel issues a **typed memory access** through a pointer the caller
chose. A typed access has an alignment precondition, and the kernel did not pick the pointer.

## Alignment is a hardware property, and it is not uniform

The C++ abstract machine says an object of type `T` lives at an address that is a multiple of
`alignof(T)`; a typed access to a misaligned address is undefined. What the hardware *does* with a
misaligned access varies by ISA, and so does `alignof(T)` itself:

- On a **strict-alignment** ISA a misaligned load/store **traps**. RV32IMAC is the fleet's
  example: a misaligned word access raises an exception. Issued from *kernel* context on a pointer
  the *user* supplied, that trap is a user-triggerable kernel fault -- a denial-of-service, not a
  mere wrong answer.
- A **relaxed-alignment** ISA (RXv3, most ARM data accesses) completes a misaligned access, but
  that does not make it free or always safe: some instructions still require alignment, and the
  compiler is only obligated to emit a working access when the pointer meets `alignof(T)`.
- Crucially, **`alignof(uint64_t)` is not the same everywhere**. It is 8 on ARMv7-M, ARMv6-M,
  RV32IMAC, Xtensa LX6, and the x86-64 host sim -- but **4 on RXv3** (the RX psABI aligns 64-bit
  scalars to 4, and KickOS's thread stacks are 4-byte aligned). So a `uint64_t` local on RX is
  *correctly* aligned at a 4-byte boundary the compiler is happy to generate two 32-bit stores
  for; it is simply not 8-aligned.

The takeaway: "aligned enough for a `uint64_t`" is a per-target fact the *compiler* knows and the
programmer usually should not hardcode.

## The two ways a boundary alignment check goes wrong

An alignment guard at the syscall boundary exists to make the *kernel's own* typed access
well-defined. It can fail in both directions:

1. **Too lax (or absent).** A genuinely misaligned pointer reaches the kernel's typed access. On a
   strict-align ISA the kernel traps (DoS); elsewhere it is silent UB. This is the failure the
   guard is meant to prevent -- and it must also cover the *struct* copy-in, not just the obvious
   64-bit out-pointer, since a misaligned struct pointer traps the kernel's field loads just the
   same.
2. **Too strict.** A guard that demands *more* alignment than the type actually needs on this arch
   rejects legitimate, correctly-aligned pointers. The syscall then refuses to do its work -- and
   because a rejected out-pointer store simply does not happen, the stub returns whatever it
   pre-initialised (for `clock_now`, a zero). The caller sees a plausible-but-wrong value with no
   error it can act on. A hardcoded 8-byte requirement does exactly this on RXv3: it rejects the
   stub's legitimately 4-aligned local, so the clock reads a frozen zero. The symptom appears far
   from the cause (a spin loop that never advances), and it is *layout-sensitive* -- whether any
   given stack local lands on an 8-boundary depends on the frame, so the same code "works" in one
   build and hangs in another.

Mode 2 is the more insidious: it looks like defensive hardening, passes on every arch where
`alignof(uint64_t)` happens to be 8, and only surfaces on the one arch where it is smaller -- an
arch that is often the last to be re-tested precisely because it is the odd one out. This is the
uniform-fleet thesis stated as a hazard: **a check written against one ISA's numbers is a latent
bug on every other ISA**, whichever direction the numbers differ.

## The discipline: state the requirement the way the compiler does

The correct requirement is not a constant; it is `alignof(T)` for the exact type the kernel is
about to access. Two properties make this the right expression rather than a per-arch table:

- **It cannot drift from the access it guards.** The kernel's store is generated by the same
  compiler, for the same target, whose `alignof(T)` the guard tests. If the guard passes, the
  compiler's own generated access is by definition satisfied. A separate per-arch constant can be
  wrong in either direction; `alignof` is right by construction.
- **It is arch-neutral source that resolves per target.** `alignof(uint64_t)` compiles to 8 on
  ARM/RISC-V and 4 on RX with no `#ifdef`, exactly mirroring how `sizeof` is used for width. The
  same reasoning extends to any future ISA: if some target aligned `uint64_t` to 2, `alignof`
  would accept 2-aligned pointers there and the compiler would be obligated to emit an access that
  works -- the guard and the codegen stay in lockstep.

So the boundary check for a `T`-typed access on a caller pointer `p` is: reject null, reject
`p & (alignof(T) - 1)`, then bound `[p, p + sizeof(T))` against the caller's granted regions. The
same three-part shape -- null, alignment, ownership -- applies to a 64-bit out-pointer and to a
struct copy-in alike.

## Alignment is one facet of "never trust a user pointer"

Alignment sits alongside the other caller-pointer hazards the kernel must neutralise at the single
validate-and-resolve chokepoint, all for the same reason -- the kernel dereferences privileged
what the caller chose:

- **Ownership** -- the range must lie within a region the caller was granted (bounds-checked
  against its MPU regions), or the kernel leaks/corrupts memory the caller could not itself reach.
- **Width** -- copy a user struct *into* kernel memory once and read fields from the copy, rather
  than re-dereferencing a pointer the caller could mutate concurrently.
- **Aliasing / lifetime** -- copy user strings into a fixed kernel buffer before use; a fault
  reporter that prints an aliased user pointer with `%s` is an information-leak oracle.

Alignment is the cheapest of these to get subtly wrong, because a plausible constant hides the
arch dependence. Express it as `alignof(T)` and it becomes as portable as the rest of the fleet.

## Ownership has two arms, and the build macro is not the key

The ownership bullet above hides a subtlety worth drawing out, because it is where a check that
looks complete turns out to be keyed on the wrong thing.

Ownership is asked as two deliberately symmetric predicates -- one for a buffer the kernel will
**read** on the caller's behalf, one for a buffer it will **write** -- and each is answered in two
arms. The first arm is the region check: does the range lie inside one of the running thread's
granted regions, with the permission the access needs? The second arm exists because *a granted
region set does not describe everything an unprivileged caller can legitimately reach, on every
backend*. Where the linker carves app code/rodata and app static data as real protection-unit
regions, the region check alone is complete and the second arm correctly refuses everything. Where
the backend models no such region -- a part with no protection unit at all, and the host sim, whose
ELF sections are not regions -- an unprivileged thread's *writable* set would be **its own stack
alone**, so an out-pointer or a receive buffer that happens to live in a global would be refused as
a bad address. The second arm is each backend answering for its own protection model.

Which is exactly why the arm cannot be keyed on the build macro that says "this build has a
protection unit". The host sim builds *with* that macro set -- it enforces via `mprotect` -- and yet
it needs the fallback arm, because what the macro asserts is that a backend exists, not that static
data is described as a region. The two are independent, and conflating them yields a check that is
sound on the boards you tested and refuses valid pointers on the ones you did not. The rule
generalises past this one predicate: **validate against the protection model the backend actually
implements, not against the configuration flag you think you built with.**

The sim's own arm is worth reading for what it admits it cannot do. It accepts a range wholly
inside the host image and clear of the emulated arena -- and the host image contains the kernel's
code and data too, because sim builds are one binary. So the sim **cannot** separate app rodata
from kernel statics; that is a property of the environment, not an omission. What it does still
enforce is the boundary that matters most there: a cross-domain read into another task's arena
memory, which lives in a separate mapping and falls through to the region check. The transferable
part is knowing *which* claims your cheapest test environment is able to carry, and not letting it
be the only witness for the ones it cannot.

One last observation, about testability rather than correctness. The write predicate is the later
twin of the read one, and its absence is invisible for exactly as long as the callers exercising
that path are privileged -- because a privileged caller short-circuits the whole check before
either arm runs (Chapter 7.4,
*[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*, axis 2). A
boundary check whose only exerciser sits on the bypass side of the boundary is not a tested check.
It is a check that has never run.

*Points into:* `../reference/architecture.md` ("Syscall-argument validation (the soundness
floor)", "User/kernel separation"); the confused-deputy floor it extends. Further reading:
Tanenbaum, *Modern Operating Systems*, ch.1 (system calls, the user/kernel boundary); an ISA's
psABI / architecture manual for its `alignof` and misaligned-access rules.
