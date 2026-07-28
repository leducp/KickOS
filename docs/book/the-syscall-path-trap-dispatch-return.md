<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# The syscall path: trap, dispatch, return

> The chapter that closes the user-to-kernel-and-back arc. Chapter 0.2,
> *[Interrupts and traps](interrupts-and-traps.md)*, introduced the trap as a
> deliberate synchronous request and named the instruction on each ISA. Chapter 3.5,
> *[Context switching and the silicon contract](context-switching-and-the-silicon-contract.md)*,
> established what a thread's saved state is, privilege included. This chapter follows
> one system call the whole way: the userspace stub, the per-arch trap entry, how the
> handler reaches `syscall_dispatch`, what the kernel must check before it touches
> anything the caller chose, how a blocking call parks the caller, and how the return
> restores the caller's privilege. It ends on the surface itself, because *which*
> operations are syscalls is a design decision and not an accident of history. Points
> into [`../reference/invariants.md`](../reference/invariants.md) ("Syscall, privilege
> & the user/kernel boundary") and
> [`../reference/architecture.md`](../reference/architecture.md) ("User/kernel
> separation") for the exact contracts.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1 (system calls and the
> user/kernel boundary).*

## The problem: an unprivileged program cannot call the kernel

An application thread needs things only the kernel can do: block until a semaphore is
free, create another thread, hand a message to a driver. It cannot simply call the
function that does the work, and it is worth being precise about why, because there
are two independent reasons and each rules out a different shortcut.

**It does not have the privilege.** The kernel's data structures live in memory the
protection unit denies it, and the operations it needs end in instructions the
architecture reserves for the privileged level (Chapter 7.4,
*[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*).

**A call is not a mode change.** Even given the address of the kernel routine, a
`bl`/`jal`/`bsr` to it changes the program counter and nothing else. It would arrive
inside kernel code still running unprivileged, still without the memory, and -- worse
-- it would arrive at whatever instruction the caller chose rather than at a door the
kernel controls. If jumping into the kernel were enough, privilege separation would
be a suggestion.

What is needed is a way for unprivileged code to ask the hardware to perform the
crossing on its behalf, entering at an address the *kernel* decided. That is exactly
what a trap instruction does, and it is why a system call is implemented as a trap.

## The design space: how should a request reach the kernel?

Assume the trap exists. There is still a choice about the *shape* of a request, and
three answers have real systems behind them.

**A synchronous trap in the caller's own thread.** The caller executes the trap; the
kernel does the work on the caller's own continuation; if the operation must block,
the caller blocks. One crossing in, one back.

**A request sent to a service thread.** The caller marshals its request into memory,
hands it to a thread that lives inside the kernel's domain, and waits for a reply.
This has an appealing "everything is a message" purity to it.

**An asynchronous submission queue.** The caller writes requests into a shared ring
and rings a doorbell; completions come back through a second ring. This is the
`io_uring` shape, and it is the right answer for high-throughput batched I/O.

The trade-offs decide it quickly for a small real-time kernel.

The service thread costs at least two context switches and a rendezvous per request
where the trap costs one crossing, and it needs the request marshalled into memory
*both* sides can reach -- which is a shared writable window, precisely the thing the
isolation model exists to avoid. It also inverts blocking semantics in an awkward way:
"block me until this semaphore is free" becomes "block me waiting for a reply from the
thread that is blocking on my behalf", so there are two parked threads where there
should be one. And the service thread would itself have to live in the kernel's
protection domain, which is a hole in the microkernel claim taken on for no isolation
gain (Chapter 7.4).

The submission queue is excellent at amortising many requests and bad at the latency
of one small operation, which is the shape almost every real-time syscall has. It also
cannot express "park this thread" at all without a second, synchronous mechanism
underneath it, so it does not replace the trap; it sits on top of one.

**KickOS uses the synchronous trap for every syscall**, and the decisive reason is not
the switch count. It is that the kernel's blocking primitives are ordinary C functions
that end in `arch_switch`. If the dispatch ran anywhere other than in the caller's own
thread context, `sem_wait` could not simply block: it would need a continuation object,
a state machine, or a reply protocol. Running dispatch on the caller's own stack makes
blocking free. That observation is load-bearing enough that it is written down as a
contract every port must satisfy.

## The contract: dispatch runs in privileged THREAD context

One invariant governs the whole path (`syscall-in-priv-thread-context`):

> The arch MUST run `syscall_dispatch()` in privileged **thread** context on the
> calling thread's own continuation, never in handler/ISR context. A blocking syscall
> blocks by an ordinary synchronous context switch that freezes the mid-dispatch
> continuation on the caller's stack and resumes it inline when the thread is next
> scheduled.

Two testable consequences follow. `arch_in_isr()` must read false throughout dispatch,
because kernel code legitimately behaves differently in interrupt context and dispatch
is not interrupt context. And a blocking syscall must actually block *before* it
returns to the caller.

That second one is why the obvious implementation is wrong. The obvious implementation
dispatches inside the trap handler: the handler has the arguments, it is already
privileged, so why leave? Because on a core where a switch requested from handler mode
can only be *deferred* -- ARM's PendSV tail-chains after the handler returns, and the
same shape holds for the RX and RISC-V software-interrupt switchers (Chapter 3.5's
deferred-switch axis) -- a blocking call made inside the handler would return to the
caller first and switch away afterwards. The caller would run on, believing it had
blocked, until the deferred switch fired. That is a lost-wakeup-class bug built into
the architecture of the syscall path.

So every port with a ring split does the same thing: **it uses the trap only to change
mode, and leaves the handler immediately.**

## Per-arch entry: five doors, one room

| Port | Trap | How the handler reaches dispatch |
|---|---|---|
| armv7m / armv6m | `svc #0` | `SVC_Handler` rewrites the *stacked* PC in the hardware exception frame to `svc_trampoline`, clears `CONTROL.nPRIV`, and exception-returns -- so the trampoline runs in privileged **thread** mode on the caller's own process stack |
| rv32imac | `ecall` | one common trap entry saves the integer file, demuxes `mcause` (`ecall`-from-U is 8, from-M is 11), then `.Lecall` points `mepc` at `svc_trampoline`, sets `MPP=M` and `MPIE=1`, and `mret`s |
| rxv3 | `int #1` | the handler stashes the user PC and PSW on the thread's own user stack, rewrites the stacked PC to `svc_trampoline` and the stacked PSW to supervisor-on-USP, and `RTE`s |
| lx6 (Xtensa) | none | the core has no ring split, so `arch_syscall` is a plain call to `syscall_dispatch`; the contract is satisfied trivially |
| host sim | none | also a plain call, bracketed by an `mprotect` posture raise for the duration |

Read the three trap-bearing rows together and the pattern is unmistakable. The trap
handler's entire job is to **arrange a privileged thread-mode continuation on the
caller's own stack, and get out of the handler**. It does not dispatch, it does not
decode the request, it does not touch a kernel data structure. It moves the return
target and the mode, and returns. Whatever the ISA calls its trap and its
exception-return, that is the shape.

Three details in that table are worth pulling out, because each teaches something the
others do not.

**On ARM, the handler edits the frame rather than jumping.** The hardware already
stacked the caller's registers on exception entry, including the arguments and the
caller's return address in `LR`. Rewriting the stacked PC and exception-returning
therefore *reuses* the hardware's own unstacking to land in the trampoline with the
arguments still in the right registers, at the right privilege, on the right stack.
Nothing is copied. The trampoline then calls `syscall_dispatch` with an ordinary `bl`,
which is exactly what makes the C dispatch a normal C function.

**On RISC-V, `.Lecall` deliberately enables interrupts for the dispatch.** It sets
`MPIE=1` so the trampoline runs with `MIE` on, because dispatch is thread-context
kernel code that may run for a while and may block; masking interrupts across it would
add the whole syscall duration to every interrupt's worst-case latency. This has a
knock-on the restore path has to respect, and the invariant `trap-return-atomic`
records it: on the way out, `mstatus` is restored (dropping `MIE` to 0) *before*
`mepc`, because a trap taken between those two writes would clobber the staged return
address and the final `mret` would jump back into the epilogue with the frame
half-popped.

**Two ports have no trap at all, and both still have syscalls.** Xtensa LX6 has no
ring split and the host sim has no CPU mode; on both, `arch_syscall` is a plain
function call. This is the clearest possible demonstration that the trap is a
*mechanism* and not the boundary itself. What actually constitutes the boundary is the
set of checks the dispatch performs. A port that cannot raise privilege still enforces
every authority gate, every handle resolve, and every scalar range check, because those
are kernel logic. (The sim adds one wrinkle worth noticing: it emulates the transient
privilege raise as a per-context counter, incremented on syscall entry and decremented
on the way out, and the count lives *per context* precisely so it survives a blocking
switch and a later resume.)

## The number and the arguments: a register ABI

The stub side is one function on every port:

```
uintptr_t arch_syscall(uintptr_t nr, uintptr_t a0, uintptr_t a1,
                       uintptr_t a2, uintptr_t a3);
```

The syscall number and four arguments travel in the ISA's own argument registers by
the platform ABI, so there is nothing to marshal: on ARM they are already in `r0`-`r3`
with the fifth word moved into `r12` so the exception frame carries it; on RISC-V they
are already in `a0`-`a4`; on RX in `R1`-`R4` with one stacked. The result comes back in
the return register.

Three properties of that choice matter.

**No buffer means no buffer to validate.** A register-passed argument list cannot be
mutated by the caller between the kernel's reads, cannot straddle a region boundary,
and cannot be a wild pointer. Every hazard in the next section exists only for the
arguments that *are* pointers, and keeping the base ABI in registers keeps that set as
small as possible.

**The number is a frozen contract.** `enum kos_syscall_nr` (`sys/abi.h`) is
append-only: never renumber an existing entry, and a number is allocated when a syscall
is *built*, not when one is designed. The numbers are the ABI between a compiled
application and the kernel; renumbering is a silent, spectacular failure mode.

**A 64-bit value is split into halves, deliberately.** `sleep_ns` takes low and high
words; a 64-bit result is written through a caller-supplied out-pointer. The reason is
not the MCU's word size, it is *uniformity*: the fleet includes a 64-bit host sim, so an
ABI that relied on `uintptr_t` being 64 bits wide would be a different ABI on different
targets. Splitting at the boundary makes the register layout identical everywhere, and
the helpers (`kos_u64_lo`/`kos_u64_hi`/`kos_u64_join`) keep the split from being
open-coded.

What this costs is that anything wider than five registers has to travel through a
pointer the caller chose -- which is where the kernel's real work begins.

## The return convention: a negated errno, and where it cannot reach

A syscall that can fail returns its error as the **negated** code, `-KOS_Exxx`; a
success is non-negative, being a handle, a count, or a byte count (invariant
`syscall-return-abi`). So `rc < 0` is unambiguously an error and can never alias a
valid result. The magnitudes mirror POSIX errno where a POSIX reading exists, which
means a reader mostly does not have to learn a new taxonomy.

The convention has exceptions, and they are worth teaching because none of them is a
matter of taste -- each is a *type* problem the sign trick cannot solve.

- **A syscall returning a pointer cannot carry a negated errno.** A negative value
  reinterpreted as a pointer is a perfectly non-null pointer, so a caller doing the
  documented `if (p == NULL)` check would sail past the error. `ram_alloc` therefore
  returns NULL on **every** failure path, the not-permitted refusal included, so that
  one check is correct.
- **A syscall returning a physical quantity has no spare negative range.** The clock
  reads return a `uint32_t` in Hz whose `0` already means unknown-or-cannot, so they
  stay out of the scheme rather than acquiring a second, contradictory error channel.
- **Zero is sometimes a legitimate success.** A zero-length console write correctly
  returns 0, so 0 cannot double as "rejected"; the reject has to be a negative code.

And one case is neither an exception nor a clean fit, which is the most instructive of
all: locking a mutex whose previous owner died returns `-KOS_EOWNERDEAD`, a negative
value that nonetheless means **the lock is held**. A sign convention buys unambiguous
error *detection*; the moment an operation has an outcome that is simultaneously an
error and a success, the convention stops being sufficient and the caller has to read
the documentation at the call. (What that outcome means for the protected data is
Chapter 2.3, *[Priority inheritance](priority-inheritance-lending-urgency.md)*.)

## Validate, then use: the kernel is on the wrong side of its own boundary

Here is the inversion that makes syscall-boundary checking non-optional, and it is the
single most important idea in this chapter.

`syscall_dispatch` runs **privileged**. It is therefore on the *bypass* side of the
very protection unit that contains everybody else (Chapter 7.4, axis 2). Every value
the caller supplied is now in the hands of code the unit does not check. A bad value
does not become a contained user-mode fault; it becomes a kernel action.

That splits into three families of hostile input, and the discipline differs for each.

**Scalars: range-check or clamp, at the top of the arm.** A priority indexes the ready
lists and shifts a `1u << prio` bitmap; an interrupt line indexes the dispatch table; a
console length bounds a loop over user memory. An out-of-range scalar is not a wrong
answer, it is an out-of-bounds access *made by the kernel*, and the kernel's own region
set permits it by construction. So enforcement does not make these checks redundant:
the unit contains a bad user **pointer**, and only these checks contain a bad user
**index** (invariant `user-args-validated-at-boundary`). The same rule covers the
number itself -- an unknown syscall number returns `-KOS_EINVAL` and never reaches an
unreachable-assert, because a user-supplied value must never be able to halt the
kernel.

**Pointers: null, alignment, ownership, and only then access.** The ordering is the
lesson. Null and alignment come first because they make the kernel's own typed access
well-defined, and a misaligned kernel load traps on a strict-alignment ISA -- a
user-triggerable kernel fault. Ownership comes next: the range must lie within a region
the caller was actually granted, or the kernel becomes a laundering service for memory
the caller could not reach on its own. Alignment and ownership have their own chapter,
7.1, *[Alignment across the syscall boundary](alignment-across-the-syscall-boundary.md)*;
what belongs here is the structural half. A struct argument is **copied into kernel
memory once** through a checked read, and every field is then read from the kernel's
copy -- so a caller cannot mutate a field between the check and the use. And the kernel
never `strlen`s a user pointer: a buffer crosses the boundary as an explicit
`(pointer, length)` pair, because taking the length from the memory being validated is
circular.

**Handles: resolve first, touch nothing until it resolves.** A syscall naming a kernel
object resolves the caller's handle through one chokepoint before it links a queue,
moves a counter, or dereferences anything -- Chapter 8.1,
*[Naming a kernel object](handles-and-the-resolve-chokepoint.md)*.

The dispatch itself is one flat switch on the number, which is what makes this
auditable: there is exactly one door, each syscall's checks sit at the top of its own
arm, and the `default` is a refusal rather than a surprise.

## Blocking syscalls are ordinary context switches

Now the payoff of the thread-context contract. When dispatch calls `sem_wait` and the
count is zero, `sem_wait` does exactly what it would do if the kernel had called it
from anywhere else: it parks the thread on a wait queue and switches away (Chapter 2.2,
*[The blocking substrate](the-blocking-substrate-one-wait-wake-primitive.md)*).

Nothing about that is syscall-specific, and that is the point. The half-finished
dispatch lives on the caller's own stack -- its ARM process stack, its RISC-V frame,
its RX user stack -- so the frozen continuation *is* part of the thread's saved state.
When the thread is scheduled again it resumes inside `sem_wait`, returns into the
middle of the dispatch, finishes the syscall, and returns to userspace. No kernel-side
state machine, no continuation objects, no reply protocol.

Two consequences are worth stating explicitly.

**The blocked thread's C call stack IS the continuation.** A design that dispatched in
an ISR would have needed to invent something to represent "this syscall is half done",
and that something would have had to be allocated, bounded, and torn down on thread
exit. Here it is free, because it is a stack frame.

**A thread can be preempted mid-syscall, so its saved privilege must be the transient
one.** Its saved CPU mode while parked inside a syscall is *privileged* -- it must be,
or the continuation would resume unprivileged and fault on the kernel data it was in
the middle of touching. What it goes back to at the end is its resting mode, held
separately in the saved context. That resting-versus-transient distinction is created here and
consumed on the way out; Chapter 7.4 explains why it is an axis and Chapter 3.5 explains
where in the saved state it lives. On ARM there is one further detail that makes the
handler-versus-thread-mode choice concrete: the deferred switch arrives as a *real
exception preempting the trampoline's thread-mode code*, which is precisely the thing
that could not happen if the dispatch were running in the handler.

## The way back: resting privilege, atomically

The epilogue does two things, and both are contracts.

**It restores the caller's resting privilege, not a hard-coded unprivileged.** The
saved context carries the caller's resting mode -- a dedicated per-thread field on ARM,
the stashed user `PSW` on RX, the saved `mstatus.MPP` on RISC-V -- and the trampoline
writes it back (invariant `syscall-restores-resting-priv`). Hard-coding unprivileged would
silently demote any privileged thread that ever made a syscall -- a latent bug that
would surface far from its cause, in exactly the threads whose posture matters most.

That epilogue also shows the asymmetry the whole security property rests on, in one
line of assembly: **lowering privilege is a plain register write, raising it requires
the trap.** On ARM the trampoline drops back by writing `CONTROL` directly, no trap
needed, because giving up privilege is always safe. There is no corresponding
instruction to take it.

**The return is atomic with respect to interrupts.** No interrupt-takeable window may
exist while the return state -- the return PC plus the saved status and mode -- is
staged but not yet consumed (invariant `trap-return-atomic`). Otherwise a nested trap
overwrites the staged return and the outer return jumps into its own epilogue at the
caller's privilege with the frame half-restored. This is an end-property, not a code
shape: ARM's exception unstack is hardware-atomic, RX pops PC and PSW in one `RTE`,
RISC-V orders the `mstatus` write before the `mepc` write as described above. Each port
closes the window its own way, and a port that invents a new return path has to close
it again.

## The surface is deliberately small

Everything above describes *how* a syscall works. Which operations get to be syscalls
at all is a separate decision, and in a microkernel it is the more consequential one.

The whole surface is a few dozen numbers, several of them compiled out of production
images: yield and sleep; create/wait/post on a semaphore, lock/unlock on a mutex,
create/send/recv/call/reply on an endpoint, and one type-agnostic close; spawn and exit;
register/wait/ack/attach on an interrupt line; a couple of clock reads; a small set of
authority-gated machine operations (Chapter 7.4); and one debug console write.

There is no `open`, no `read`, no `socket`, no filesystem, no name service. The rule
that produces that list is worth stating as a rule:

> An operation is a syscall only if it needs something only the kernel has.

What only the kernel has is the scheduler, the blocking primitives, the protection
unit, and the interrupt plumbing. A filesystem needs none of those, so it is a
userspace server reached by IPC, and `open` is a message to that server rather than a
trap. The toolchain's libc still needs those symbols to link, so the bottom-edge
file-descriptor operations exist as stubs; a real implementation belongs to whichever
server owns the device, on the other side of an endpoint. That is not a limitation
being apologised for -- it is the microkernel thesis applied to the syscall table.

The **one sanctioned kernel exception is the debug console write**, and its
justification is specific rather than general. The console is the panic path and the
bring-up path. A system whose only output route is IPC to a userspace driver cannot
report a failure that happens *before* that driver exists, or one that happens *inside*
it -- and those are exactly the failures you most need to read. So the kernel keeps a
console write, explicitly as a debug affordance rather than a general I/O API, and it
is the one place the kernel streams a user buffer privileged instead of copying it
through the bounce seam (which is why it clamps the length and bounds the buffer against
the caller's own regions before reading a byte).

The libc bottom edge shows the intended shape working end to end: `_write` prefers IPC,
sending to the calling thread's own stdout capability, and falls back to the kernel
console only when there is no such capability or the driver is gone. The kernel path is
the fallback, not the road.

## What it costs, and when not to use it

A syscall is a trap, and a trap costs what Chapter 3.5's cost section describes: the
hardware entry, whatever register saving the core does not do for free, the trampoline
redirect, and the return. For a control-plane operation -- create a thread, block on a
semaphore, send a message -- that cost is irrelevant next to what the operation itself
does.

For a *hot* operation it can be the whole cost, and then the right answer is not to
make the syscall faster. It is to notice that a granted MMIO window **is** the
authority, so the operation needs no kernel involvement at all: the capability is the
fast path, and putting a syscall on top of it only re-pays a check that was already
made at grant time. Chapter 8.4,
*[The fast path is the capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*,
works that argument through on a chip-select toggle with a 222 ns budget. The small
syscall surface is not only a purity argument; it is where the performance argument
lands too.

## Where to go next

- The trap-versus-interrupt distinction and the vector table this rides on: Chapter
  0.2, *[Interrupts and traps](interrupts-and-traps.md)*.
- The saved state the trap freezes and restores, and what a switch costs: Chapter 3.5,
  *[Context switching and the silicon contract](context-switching-and-the-silicon-contract.md)*.
- The park/wake primitive a blocking syscall lands in: Chapter 2.2,
  *[The blocking substrate](the-blocking-substrate-one-wait-wake-primitive.md)*.
- The pointer half of validate-then-use: Chapter 7.1,
  *[Alignment across the syscall boundary](alignment-across-the-syscall-boundary.md)*.
- What "privileged" means axis by axis, and the authority gates in this surface:
  Chapter 7.4, *[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*.
- The handle-resolve chokepoint every object-naming syscall funnels through: Chapter
  8.1, *[Naming a kernel object](handles-and-the-resolve-chokepoint.md)*.
- Each ISA's concrete trap entry, worked end to end: Chapter 4, *Per-ISA guided tour*,
  and the fully worked port
  [`porting-a-new-isa-riscv.md`](porting-a-new-isa-riscv.md).
- The exact contracts: [`../reference/invariants.md`](../reference/invariants.md)
  ("Syscall, privilege & the user/kernel boundary") and
  [`../reference/architecture.md`](../reference/architecture.md) ("User/kernel
  separation").
