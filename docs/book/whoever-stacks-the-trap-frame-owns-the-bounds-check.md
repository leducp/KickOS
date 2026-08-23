<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Whoever stacks the trap frame owns the bounds check

> A Chapter-7 companion about the one pointer a trap has no choice but to use. When a CPU
> takes a trap, something must save the interrupted registers, and the only stack pointer
> available at that instant is the one the interrupted code left behind. On a microkernel
> whose threads run unprivileged, that register is *untrusted input*. Whether anything
> checks it depends on *who does the saving*, and that single question splits a uniform
> fleet into four genuinely different exposures. The chapter then works through the four
> ways the check itself goes wrong: guarding the wrong path, testing a pointer instead of
> an extent, bounding the frame instead of the whole descent, and writing down a margin
> nobody re-measures. It extends Chapter 3.9
> ([*The syscall path*](the-syscall-path-trap-dispatch-return.md)) and Chapter 7.4
> ([*Privilege is three axes, not one bit*](privilege-is-three-axes-not-one-bit.md)), and
> binds to [`../reference/invariants.md`](../reference/invariants.md)
> (`fault-frame-untrusted-until-proved`, `syscall-in-priv-thread-context`,
> `mpu-apply-on-every-switch-in`) plus the three per-arch trap-stack headers under
> `arch/arm/armv7m/include/kickos/arch/`, `arch/riscv/rv32imac/include/kickos/arch/` and
> `arch/rx/rxv3/include/kickos/arch/`.

## The problem: the frame has to go somewhere, and only one pointer is available

A trap is an involuntary function call (Chapter 0.2). The core stops the running
instruction stream at an arbitrary instruction and transfers to a handler, and the
handler needs the registers for its own work. So the interrupted values have to be
written to memory before the handler can use those registers, and they have to be written
somewhere the eventual return can find them again.

There is essentially one candidate: the stack. It is per-thread, it grows away from the
thread's live data, and the thread's own resume point is already expressed relative to
it. Every architecture in this book puts the trap frame on a stack.

Now ask *which* stack, and the answer at that precise instant is uncomfortable. The
handler has not yet run a single instruction of its own, so it has not yet consulted the
scheduler, read a thread control block, or done anything at all. The only stack pointer
that exists is the one the interrupted code was using. On a system where the interrupted
code is an unprivileged thread, that pointer is a value the thread chose. A thread can
put any 32-bit number in its stack register: it takes one instruction, needs no
privilege, and no protection unit anywhere refuses it, because *moving* a pointer is not
an access.

So the trap frame is written through a pointer supplied by the untrusted party. That is
the whole problem, and it is worth stating in the same shape as Chapter 3.9's
validate-then-use rule for syscall arguments:

> The stack pointer at trap entry is a syscall argument that arrives before the syscall,
> through a register nobody declared, and gets used before any code has run that could
> check it.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (the trap as the protection
boundary) and ch.9 (why a protection domain is defined by what it can name, not by what
it intends).*

## Why this is escalation, and not merely corruption

A wild pointer usually means a crash. This one does not, and the reason is the whole
point of the chapter.

The trap prologue runs at the *privileged* level, because that is what a trap is for. And
at the privileged level the protection unit is, on every backend in the fleet, either
bypassed outright or given a permissive background. Each architecture reaches that state
by its own mechanism:

- **RISC-V.** Physical Memory Protection constrains machine mode only for entries whose
  lock bit is set. KickOS sets no lock bit anywhere: `pmp_cfg` builds a configuration
  byte out of the NAPOT address-matching field plus the read, write and execute bits the
  *U-mode* grant needs, and the L bit is never in the set. Machine mode therefore reaches
  all of memory. That is deliberate: it is how the backend produces the permissive
  privileged background the ARM ports get from the architecture (see
  `mpu-bypass-windows-accepted`).
- **RX.** The RXv3 MPU has no supervisor permission field at all. It checks user-mode
  accesses and nothing else, so a supervisor access is unconditionally permitted whatever
  the region registers hold.
- **ARM.** PMSA gives a privileged thread a permissive background region
  (`mpu-apply-on-every-switch-in`), and handler mode is privileged.

So a store executed by the trap prologue through the thread's chosen pointer is not a
faulting access. It is a *successful* write, at an address the unprivileged thread named,
of data the unprivileged thread largely controls, performed by the kernel. That is the
textbook definition of a privilege escalation primitive: an arbitrary write. Everything
after it is a matter of what the attacker chooses to overwrite.

And the escalation does not even need a kernel target. Thread stacks come from a bump
allocator with no padding between equal-sized blocks (Chapter 3.6), so the memory
immediately below one thread's stack base is *another thread's granted region*. A frame
that lands there is written into memory a second unprivileged thread can read and write
with ordinary stores, no privilege required. Two cooperating unprivileged threads are
enough: one aims its stack pointer to make the kernel deposit a return descriptor in the
other's memory, the other edits it. Nothing in that sequence touches a privileged
address.

## The design space: three places a frame can land

There are only three answers, and each fleet member ends up at some combination of them.

**Option 1: the hardware does it, at the pre-exception privilege.** The architecture
defines exception entry as writing a fixed frame through the thread's stack pointer,
*checked as if the thread had done it*. A kernel-aimed pointer then faults on entry,
before the handler exists. This is free and it is airtight, but only an architecture can
offer it, and only for the frame the architecture defines. Anything the kernel pushes in
addition is outside the guarantee.

**Option 2: a trusted per-core trap stack.** The prologue's first act is to swap onto a
stack the kernel owns, which no thread can name. Nothing untrusted is ever stored
through. The cost is that the trap frame is then not on the thread's stack, which matters
enormously for the syscall path: Chapter 3.9's contract
(`syscall-in-priv-thread-context`) requires the kernel's dispatch to run on the *calling
thread's own continuation*, so that a blocking syscall blocks by an ordinary context
switch. A frame on a shared per-core stack cannot be frozen and resumed that way, because
the next trap on that core would land on top of it.

**Option 3: the thread's own stack, validated.** Adopt the interrupted pointer, but only
after proving it names room inside the thread's own recorded stack. This costs a handful
of instructions on every trap and it costs a *correct check*, which is where the rest of
this chapter lives.

KickOS takes option 1 wherever the architecture provides it and option 3 everywhere the
kernel writes bytes of its own, which is everywhere. Option 2 is not a third posture so
much as the precondition option 3 needs: a validation step has to run *somewhere*, and it
needs registers, so the prologue must already be standing on trusted memory before it can
refuse anything. The order is the whole trick. A prologue that validates before it has
somewhere safe to stand has nowhere to spill the scratch it needs; a prologue that adopts
first and validates second has already written the frame.

The two families reach that precondition differently, and it is worth seeing that they are
the same idea. On ARM the handler is standing on trusted memory *by architecture*: handler
mode forces the main stack pointer, and the caller's scratch registers are already in the
hardware frame, so the guard can borrow two of them freely and the untrusted PSP is just a
value it inspects. On RISC-V nothing is free and the prologue has to manufacture the same
situation, which it does in one instruction:

```
trap_entry:
    csrrw   sp, mscratch, sp     # swap to the trusted per-hart trap stack
    ...                          # scratch registers spill HERE, not on the thread's stack
    ...                          # if the trap came from U-mode, validate the swapped-out sp
    csrrw   sp, mscratch, sp     # adopt it, only on success
```

`mscratch` holds the top of a small kernel-owned trap stack. The swap is a single
instruction that both parks the untrusted pointer somewhere retrievable and installs a
trusted one, which is exactly what a validation step needs to exist at all.

## Four ISAs, four exposures

This is the part that repays reading across the fleet, because the same kernel design
meets four different amounts of help from the silicon.

| Port | Who stacks the entry frame | Who checks the pointer | What is left unchecked without a kernel guard |
|---|---|---|---|
| armv7m | hardware, through PSP, at the pre-exception privilege | the MPU, reported as `MSTKERR` | the kernel's own `{r4-r11, EXC_RETURN}` push below it, and the privileged descent below that |
| armv6m | hardware, through PSP, at the pre-exception privilege | the MPU where the part has one, and nothing where it does not (no fault-status register to say which) | the kernel's `{r4-r11}` push, which carries no return descriptor, and the privileged descent below it |
| rv32imac | software, in machine mode | nothing | the entire frame, and the privileged descent below it |
| rxv3 | software, in supervisor mode | nothing | the entire frame, and a supervisor trampoline running on that same stack |
| lx6, host sim | not applicable | not applicable | nothing: there is no privileged level to escalate *to* |

### armv7m: the hardware checks exactly the half it wrote

ARMv7-M exception entry stacks eight words -- `{r0-r3, r12, LR, PC, xPSR}` -- through PSP,
and it does so at the privilege the core held *before* the exception. So an unprivileged
thread that aims PSP at kernel memory does not get a kernel write; it gets a MemManage
stacking error, `CFSR.MSTKERR`, before the handler's first instruction. That is option 1
working perfectly, and KickOS leans on it: the fault reporter's early-out on the CFSR
stacking-error bits (mask `0x1818`) exists precisely because that abort is a real,
expected outcome (`fault-frame-untrusted-until-proved`).

Then the kernel pushes more. Both guarded sites in `arch/arm/armv7m/switch.S` -- the
PendSV switcher and the SVC trap -- extend the frame downward with the callee-saved
registers the hardware did not take:

```
    tst     lr, #0x10               /* EXC_RETURN bit4 == 0 => FP frame is live */
    it      eq
    vstmdbeq r0!, {s16-s31}
    stmdb   r0!, {r4-r11, lr}       /* callee regs + EXC_RETURN */
```

Count it: `{r4-r11}` is eight registers plus `lr`, so nine words, **36 bytes**. On an
FP-capable build `{s16-s31}` is sixteen more words, **64 bytes**, so the software block
reaches **100 bytes** in the FP-live posture. The header
`arch/arm/armv7m/include/kickos/arch/armv7m_trap_stack.h` pins those figures and the
backend static-asserts them against the register counts they price.

Every byte of that is *below* the hardware frame, written in handler mode, and refused by
nothing. A PSP placed a few words above a stack's base clears the hardware check --
the eight-word frame fits -- and the software push still lands under the stack. The
arithmetic of the attack is unglamorous: 32 bytes of room is enough for the hardware and
not enough for the kernel.

And the last register in that store list is `lr`, which in handler mode holds EXC_RETURN:
the descriptor the epilogue branches through to leave the exception. The epilogue reloads
it from memory and returns on the loaded value:

```
    ldmia   r0!, {r4-r11, lr}       /* pop callee regs + EXC_RETURN */
    ...
    bx      lr                      /* resume the incoming thread */
```

Nothing between the load and the branch validates it. So the word is a *steering* word:
`0xFFFFFFF1` in that slot resumes the victim thread in handler mode, privileged, on the
main stack. One word, written into a neighbour's memory by the kernel itself, converts an
unprivileged thread into a privileged one.

A detail worth getting right, because it is easy to state backwards. `stmdb` decrements
the address and then stores in *increasing register order to increasing addresses*, so
`lr` is the last register stored and lands at the *highest* address of the software
block, immediately under the hardware frame. It is the last word written and the word
*furthest* from the stack base. That is one more reason the check has to be about extent
rather than about where the pointer sits: the exploitable word is the one that ends up
deepest inside the neighbour, and the pointer that puts it there is the one closest to
being legal.

### armv6m: a weaker consequence is not a smaller hole

The v6-M switcher pushes the same callee-saved registers, and it has no FP bank, so its
software block is 32 bytes of `{r4-r11}` with no descriptor word. The epilogue does not
reload a return descriptor at all. It rebuilds one from a literal:

```
    ldr     r0, =0xFFFFFFFD         /* thread mode, PSP */
    mov     lr, r0
    bx      lr
```

Nothing on the thread's stack participates in the mode the core returns to. The
privilege half of the class simply does not exist on that backend, and it does not exist
because the architecture has no FP frame to select and therefore no reason for the port to
keep a descriptor in memory. A weaker ISA came out safer, for a reason unrelated to
safety.

That is a genuinely useful observation and also a trap, so hold both halves of it. The
*honest* reading is that immunity here is a property of a specific instruction sequence,
not a property the architecture promises. If a v6-M port ever gained a reason to store a
return descriptor, or to stash any other resume-steering word in the software block, the
class would arrive with it and nothing would announce the change. "Safe by accident" is a
statement about today's code, so it belongs in a comment next to the code that makes it
true, which is where the fleet keeps it. What it is not is a reason to believe the
architecture is doing you a favour.

It is also worth being clear about what the immunity does and does not cover. The 32-byte
push can still land under the stack and still overwrite a neighbour's memory; what it
cannot do is steer anyone's privilege, because none of those eight words is consulted by
the return. That is a real reduction in severity, from escalation to cross-thread
corruption, and it is worth naming precisely because a severity reduction is the easiest
thing in this whole area to mistake for a fix.

That mistake was made here. A severity argument this shape reads like a scope decision,
and for a while it was treated as one: the privilege half was refuted, the refutation was
allowed to stand for the finding, and the port carried no bound. The reasoning was sound
and the conclusion was wrong, because the question a bound answers is not "can this steer
privilege" but "can this write outside the thread". Cross-thread corruption is the whole
of the isolation claim a protected kernel makes; a neighbour's saved PC and status word
sit in a neighbour's own granted region, and two registers of the eight land exactly on
them. So every backend whose software block goes below a pointer the thread chose carries
the same extent bound, and the ones that differ differ only in what a successful write
buys the attacker.

The durable lesson is about the shape of the argument rather than about v6-M. "It cannot
escalate" and "it cannot write outside the task" are different claims, they have different
evidence, and a proof of the first is not a proof of the second. When they get conflated
the conflation is invisible, because the code that would have contradicted it is exactly
the code nobody wrote.

### rv32imac: nothing is checked, because nothing is hardware

RISC-V has no architectural exception frame. On a trap the core writes `mepc`, `mcause`
and `mstatus`, and that is the whole of its state-saving contract. Every register is still
live and every one of them is the software's problem. So the prologue is a long store
sequence -- 28 general-purpose registers plus the saved PC and status word, inside a
128-byte frame (`KICKOS_RV_TRAP_FRAME`) -- and all of it executes in machine mode,
bypassing the unlocked PMP entries.

There is no half of this that the hardware checks. Option 1 is unavailable, which is why
the `mscratch` swap above is not an optimisation on that backend but the only thing
standing between an unprivileged thread and 128 bytes of chosen-address kernel write.

The frame's contents are worth a glance, because they show that a frame need not carry an
obvious steering word to be dangerous. It holds `mepc` and `mstatus`, so it holds the
return address and the return privilege directly. It excludes `gp` and `tp`, which are
link-time and boot-time constants respectively, and it carries no floating-point state,
because RV32IMAC has none.

### rxv3: the trampoline is worse than the frame

RX gives a little help and then takes more away. Interrupt acceptance pushes PC and PSW,
and it does so on the *interrupt* stack pointer, because acceptance clears `PSW.U`
regardless of which stack the interrupted code was on. So those two words are safe, and no
RX interrupt ever builds a frame on a thread-chosen stack at all. Every general-purpose
register is still live, though, so the port has two software prologues on the thread's own
stack: the deferred-switch handler, which saves the full context (the register file, the
floating-point status word, both accumulators, and the double-precision bank where the
build has one, well over two hundred bytes), and the syscall trap.

The syscall trap stacks almost nothing, and is the worse of the two. As Chapter 3.9
describes, it arranges a privileged thread-mode continuation by editing the stacked state
and returning into it:

```
    mvfc    usp, r15
    sub     #8, r15                 ; room for the stashed user PC and PSW
    ...
    mvtc    r15, usp                ; commit the grown USP
    mov.l   #_svc_trampoline, r14
    mov.l   r14, [r0]               ; stacked PC := the trampoline
    and     #MASK_NOT_PM, r14       ; clear PSW.PM -> SUPERVISOR
    or      #BIT_U, r14             ; set PSW.U   -> on the thread's own USP
    rte
```

Read the last three lines together. The `rte` lands in `_svc_trampoline` running
*supervisor*, on the *user stack pointer* the thread supplied, and the trampoline then calls
`syscall_dispatch`. Only two words were ever stacked, so a bound written for the frame
would be almost nothing. The real exposure is every byte of kernel C the dispatch descends
through: all of it privileged, all of it at addresses the thread named, and none of it
visible to an MPU that checks user mode only. This backend is where the difference between
bounding the frame and bounding the descent is not a refinement but the entire question.

### The two ports with no ring

Xtensa LX6 and the host sim have no privilege split (Chapter 7.4, axis 1). There is no
privileged level for a controlled write to escalate *to*, so the class does not exist
there. This is not a gap in those ports; it is the same observation Chapter 3.9 makes
about their syscall path, that the trap is a mechanism and not the boundary. It is worth
naming explicitly all the same, because "this backend is not affected" is a claim that
should rest on an argument rather than on nobody having thought about it.

## A test on the fault path is not a test on the entry path

Before the check itself, one structural mistake that is much easier to make than it looks.

A kernel that reports faults already needs a version of this test. When a fault handler
wants to print the faulting PC, it has to read a frame that lives in memory on the
faulting thread's stack, and that frame is exactly as untrustworthy as the pointer it was
written through. So `kickos_fault_frame_trusted` exists, in `kernel/init/fault.cc`, and
the porting contract insists a backend gate on it before reading a single word
(`fault-frame-untrusted-until-proved`). Every backend's `arch_fault_is_user_thread` calls
it, and the RX one carries a comment that names the whole hazard in as many words: the
thread's stack pointer is where the exit stub will run after the return, so a thread that
wrecked that register would otherwise put privileged code on a stack of its choosing.

That comment is correct, the test behind it is correct, and neither of them closes the
class in this chapter. The reason is timing. The fault path runs *after* an access has
already been refused. The entry path runs *before* any access has happened. A test on the
first says "do not believe what is in this memory"; the class needs one that says "do not
write to this memory". They are different questions, asked at different instants, and one
does not imply the other.

The transferable form of that, because it costs nothing to apply and it is where a
project's own knowledge tends to be sitting unused:

> A hazard named in a comment is a claim about a *mechanism*, not about the one call site
> where you wrote it down. If a comment explains why path A must not trust the stack
> pointer, every other path that touches the stack pointer inherits the argument. Grep
> your own warnings and ask which paths they apply to, not which path they are attached
> to.

## A pointer test is not an extent test

The obvious check is a range test: prove the pointer is inside the thread's own stack.

```c
if (sp < stack_lo or sp >= stack_hi) { refuse(); }   /* NOT sufficient */
```

It is not enough, and the failure is at the boundary rather than out in the weeds. A
pointer exactly at `stack_lo` is inside the range by any reading, and the frame is built
*downward* from it, so every byte of the frame lands below the grant. The test passes and
the write is entirely outside the thread's stack. There is nothing exotic about the
input: it is the smallest legal value of a legal range.

Combined with the bump allocator's lack of padding, that is the neighbour-thread write
from the top of the chapter, reachable with an in-range pointer.

The correct test is about the *extent* the prologue is going to touch, in the direction it
is going to touch it: how much room remains below the pointer. ARMv7-M's `PSP_GUARD` macro
in `arch/arm/armv7m/switch.S` is three legs, and the order of the first two is load-bearing:

```
    ldr     r12, [r2, #F_CTX_STACK_LO]
    cmp     r0, r12
    blo     .Lpsp_wild              /* leg 1: psp >= stack_lo */
    sub     r12, r0, r12            /* the room REMAINING below the psp */
    cmp     r12, r1
    blo     .Lpsp_wild              /* leg 2: that room covers what we will push */
    ldr     r12, [r2, #F_CTX_STACK_HI]
    cmp     r0, r12
    bhs     .Lpsp_wild              /* leg 3: psp < stack_hi */
```

Leg 2 is the extent test, and it is written as a subtraction taken *only after* leg 1 has
proved the subtraction cannot go negative. The alternative spelling, comparing `psp - need`
against `stack_lo`, computes the same thing and wraps under the base for a small `psp`,
which is a check that refuses nothing at exactly the input you most wanted it to refuse.
The RISC-V and RX prologues take the same care, and each records the same argument beside
the code: the subtraction is only safe once the lower bound is proved.

The fault path states the same rule in C, and the two are worth reading side by side
because they are the same rule pointing in opposite directions. `kernel/init/fault.cc`
writes it as one expression:

```c
return f >= lo and f < hi and bytes <= (hi - f);
```

with the reason attached: it is written on the room *remaining* rather than as
`f + bytes <= hi` so that a frame pointer near the top of the address space cannot pass
by overflowing the addition. Note that this is the *upward* form of the same overflow
worry, because a fault frame is read upward from its base while a trap frame is written
downward from the stack pointer. Both directions have an overflow, and in both the fix is
to phrase the comparison as room-remaining rather than as an endpoint you compute.

> A bounds check on a pointer you are about to write *through* is not a question about the
> pointer. It is a question about the extent, in the direction of travel, phrased so the
> arithmetic that answers it cannot wrap.

## The frame is not the whole descent

Bound the frame correctly and a second hole is still open, and it is much deeper than the
first.

The reason is a deliberate design choice, and Chapter 3.9 argues for it at length: KickOS
runs `syscall_dispatch` on the calling thread's own continuation
(`syscall-in-priv-thread-context`, and the `arch_syscall` contract in
`arch/include/kickos/arch/arch.h`). The trap handler does not dispatch. It arranges a
privileged thread-mode continuation on the caller's own stack and gets out of the handler,
so that when a syscall blocks, it blocks by an ordinary context switch that freezes the
mid-dispatch continuation on that stack and resumes it inline later.

That is the right choice for the reason 3.9 gives, and it is what makes this hole deep.
The privileged code running on the thread-chosen stack is not 36 or 128 bytes of frame. It
is the whole kernel C call tree below the trampoline: dispatch, the syscall body, thread
creation, the grant admissibility walk. Hundreds of bytes, privileged, at addresses the
thread named, with the protection unit not consulted.

So the bound has to cover the frame *plus* the deepest descent that trap can reach. In
practice that means the guarded sites do not all charge the same figure, and the split is
instructive:

- **The switcher's push** charges the frame and nothing else. On ARMv7-M, `PendSV_Handler`
  runs in handler mode, where the architecture forces `SP_main`, so every call it makes --
  the MPU commit, the telemetry hooks -- descends on the main stack rather than on the
  thread's. Its kernel-descent term is zero, and that is a *claim about handler mode*
  rather than a measurement, which is a much stronger thing to rest on.
- **The syscall trap** charges the frame and the dispatch, because it is the one site that
  deliberately leaves handler mode and continues privileged on the thread's stack.

Folding the two together would reserve the syscall depth at the bottom of every stack for
threads that only ever get preempted. Keeping them apart is the reason the guard is
parameterised by *how much room this site needs* instead of by a single global constant.

### The alternative: a kernel stack per thread

The other answer to this whole section is to stop running kernel code on user-chosen
memory at all: give every thread a second, kernel-owned stack, switch to it on trap entry,
and run dispatch there. The frame and the descent both land in memory no thread can name,
and the bound disappears along with the class.

It is a real option and it is what a general-purpose kernel does. The cost is what a
small part cannot afford. A kernel stack per thread is `threads * depth` bytes of RAM
reserved unconditionally, which is precisely the static-provisioning tax Chapter 3.6
rejects for user stacks, and for the same arithmetic: on a part with tens of kilobytes,
provisioning a few hundred bytes per thread for a descent most threads never make is a
visible fraction of the chip. Running on the caller's own stack costs zero bytes. What it
buys with that zero is a bound, a guard on every trap, and a gate to keep the bound
honest.

The trade also moves with the hardware, which is worth seeing because it explains why the
answer is not universal. With a *page granule* and an MMU the arithmetic changes
completely: you leave one unmapped page below each stack, and the descent's bound becomes
a hardware fault instead of a number. You pay a page of address space, which virtual
memory makes nearly free, and you get an exact check with no measurement and no
maintenance. Without a granule there is no unmapped page to leave, because the memory
below a stack is real and belongs to a neighbour, so the check has to be arithmetic and
the arithmetic needs a figure.

### An exception can land on the descent

One more term belongs in the bound, and it is easy to miss because it is nobody's frame.

While the privileged dispatch is descending on the thread's stack, the thread is, as far as
the core is concerned, in thread mode. So on ARM a device interrupt or a tick that fires
*there* is stacked by the hardware on the process stack pointer, which is to say at the
bottom of the descent, and a deferred switch behind it pushes its software block one level
lower still. Neither is checked by anything: the guard ran once, on entry, hundreds of
bytes higher up.

Two things about that term are worth taking away.

It is **structural rather than measured**. Its writers are the hardware and one assembly
prologue, so no call-graph tool can see it, and folding it into the measured figure would
let a growing dispatch quietly eat the allowance while the measurement still reported room
to spare. The two halves are separate constants that get added, for exactly that reason.

And it is **finite only because of an argument**. A nesting term with no ceiling is not a
number. The ceiling here is that a deferred switch switches away and pops its block on
resume, and any further exception nests on the main stack, so at most one exception is ever
stacked on a thread's own stack pointer. Whether the term exists at all is per-ISA and
follows the same "who does the stacking" question as everything else in this chapter: on
RX it does not arise, because interrupt acceptance clears the user-stack bit and takes every
interrupt on the interrupt stack, so no RX interrupt can land on a thread's stack however
deep the dispatch has gone.

## A margin nobody re-measures is the next defect

Everything above lands on a *number*: how much room the prologue demands before it agrees
to write. And a number in a header is a claim about code somewhere else. It was true when
it was measured. The dispatch it prices gains a call, an inlining decision changes, a
compiler release spills one more register, and the number is quietly wrong. Nothing fails
loudly at the moment it becomes wrong, which makes it the worst kind of defect: a security
guarantee that decays into a comment.

The only durable answer is to stop writing the number down as an opinion and start
deriving it. `tests/static/check_trap_redzone.sh` does that on every run: it cross-builds
the tree for the arch under test with `-fcallgraph-info=su,da`, which makes the compiler
emit its own per-function frame sizes and call edges, merges the graph, and takes the
longest *weighted* path from the root the trap continues into. Then it compares the
measurement against what the header reserves and fails when the measurement is larger, so
the slack cannot be spent silently.

Two properties of that gate are the transferable part, and neither is about stack depth.

**It refuses to answer rather than guessing.** A longest-path walk over a call graph is
only a bound if the graph is complete, and several things make it incomplete: an indirect
call whose target set is unknown, a reachable cycle (over which "longest path" is not a
number), a dynamically sized frame, a reachable function the compiler reported no size
for, a symbol with two definitions so the walk cannot tell which body it is pricing, and
a declared root that resolves to nothing and would therefore measure zero and always pass.
Any of them and the gate fails, naming what it could not see. On this fleet the winning
chain genuinely runs through an indirect call -- a scheduler policy hook table -- and the
fix is not to ignore it: `tests/static/trap_redzone_indirect.txt` binds each such site to
the slot its source line actually calls, and the gate stays refusing while any reachable
site is unbound, or while a binding names a callee the graph no longer contains. A static
bound checker that produces a plausible number when it cannot see the whole graph is worse
than no checker, because it launders a guess into a guarantee. That is also why the gate
scrapes the reserved figures out of the arch header as plain integers rather than
restating them: a checker holding its own copy of the thing it checks proves only that its
copy is self-consistent.

**It measures every board on the arch, not one.** The deepest chain is not a property of
the instruction set. It runs through chip-specific tails -- the reserved-block walk a
particular chip declares, the code a particular console driver pulls in -- so two boards on
the same ISA measure different depths, and a bound derived from the shallowest one is not a
bound at all. The gate takes the deepest over every preset the arch declares for itself,
which also means adding a board is an event that can move the figure. A single constant
protecting a whole ISA has to be measured against the whole ISA.

Deriving a figure, rather than choosing one, tends to expose the relations around it. Two
are worth generalising:

- **A floor must dominate the zone it is a floor for.** If the smallest stack the system
  will spawn is smaller than the red zone the prologue demands, then a thread at the floor
  does not overflow on its first syscall; it is *refused* every time, which is a
  deterministic denial of service dressed as a security check. The two constants live in
  different files and neither one's author is looking at the other, so the relation has to
  be asserted somewhere: `arch/arm/armv7m/arch_armv7m.cc` static-asserts
  `KICKOS_MIN_STACK_SIZE` against the syscall red zone, and the gate re-checks the same
  relation per preset out of the generated configuration. The direction of the fix is the
  part worth stating in the failure message, because both ends are editable and only one is
  a fact: raise the floor, and never shrink the zone to fit, because the zone is a
  measurement and the floor is a policy.
- **A measurement rig must measure the machine you ship.** A cross-build harness that sets
  a flag variable by *replacing* it inherits none of the toolchain file's own baseline --
  the target flags, the ABI, the ISA string -- and quietly measures a different machine
  than the one the number will protect. The fleet is a good illustration of how badly that
  fails to announce itself, because the same mistake produced three different symptoms on
  three targets: on one the compiler's default ISA happened to equal the intended one, so
  the number was right by luck; on another the default silently dropped an instruction-set
  revision and a floating-point unit, so the number was wrong and plausible; on the third
  the default was a different instruction encoding altogether, so the build failed
  outright. Only the third symptom is a symptom. The rule that follows: a rig combines with
  the toolchain's own initialisation rather than overwriting it (an environment variable
  rather than a cache override, in this build system), and then *re-reads the configured
  cache* to confirm every ISA token it expected is actually present, because a measurement
  of the wrong machine is the one failure mode such a gate cannot see in its own output.

There is also a residual, and the honest move is to write it down rather than to round the
number up until it disappears. The deepest reachable chain from a trap includes the
panic tail, which every kernel assertion can reach. Charging it takes the red zone above
the smallest stack the fleet will spawn, so it is excluded, and the consequence is stated:
if an assertion fires while a thread is parked at the very bottom of its red zone, the
console writer descends a bounded number of bytes below the stack base, privileged, while
the system is already terminating. That is a stated, bounded, measured cost with the
system on its way down, which is a different object from an unbounded write with the
system running. The gate prints both figures so the exclusion cannot go quiet.

## Proving it: two ways to write a witness that proves nothing

A guard is a refusal, and a refusal is only tested by an input that reaches the leg you
mean. Two mistakes make a green run meaningless, and both are specific enough to be worth
naming.

**Aim at the low edge, not out into the weeds.** A witness that points the stack pointer
at kernel `.data` is far outside the range, so it is refused by the *pointer* legs. It
tells you the guard exists and nothing at all about whether the extent leg works, which
means it passes while the interesting case -- a pointer just barely inside the range, with
not quite enough room below it -- is wide open. The interesting input for a range check is
always the endpoint.

Both witnesses in the fleet are built one image per arm for that reason, and each arm is
placed so it is reachable *only* through the leg it names, so a mutation to one leg breaks
exactly one arm. The low-edge arms are the ones worth studying. Under
`user/apps/common/faultsurvive/` the low-edge arm parks the stack pointer inside the
thread's own stack with room for the frame and nothing for the descent, and its placement
is pinned from both sides by static assertions: at least the frame's width, so the frame
itself stays in bounds and the frame-validity test cannot be the thing that refuses it, and
strictly less than the red zone, so the guard *can* refuse it and the arm is capable of
going green. An arm not bracketed that way drifts into being refused by a neighbouring leg
and stops testing anything. Under `user/apps/common/pspguard/` the equivalent arm places
the pointer with one word *more* than the software push needs, so the push leg accepts and
only a bound that also charges the kernel descent can refuse; the nearby arm that is short
of room for even the push cannot stand in for it, because it is refused earlier and so says
nothing about whether the descent is charged at all.

**Give a refusal witness a positive tell.** A guard that refuses produces a panic, and a
panic is consistent with the guard working *and* with the kernel having written below the
base first and refused afterwards. The absence of visible corruption is not evidence. So
the low-edge arm lays out its allocation as padding, then a band, then the stack, with the
band ending exactly at the stack base, grants itself only the band, fills it with a
recognisable pattern, and checks the pattern after the run. A surviving pattern is a
positive statement that nothing privileged descended past the base. The general form:
when you are testing that something did *not* happen, own the memory where it would have
happened and poison it.

Note the second-order care the out-of-range arms need. They aim into memory *granted* to
the thread, so that hardware stacking succeeds and the software push is the thing that gets
refused. Aim them at ungranted memory and the hardware refuses first, and the arm silently
stops testing the kernel's guard at all.

## A leg the architecture makes inert

One last asymmetry, because it is the kind of thing a reviewer asks about and a fleet gets
wrong in the direction of looking thorough.

The RISC-V and RX prologues both check
alignment before they check bounds, and the reason is not corruption. An in-bounds but
misaligned stack pointer makes every store in the frame misaligned; on a core that traps
misaligned stores, the resulting trap re-enters the prologue, rebuilds the frame one frame
lower, traps again, and descends forever with no watchdog. That is a real hazard and the
leg belongs there.

The same leg on ARMv7-M has nothing to do, and the argument is architectural: ARMv7-M
defines `SP[1:0]` as read-as-zero, write-ignored, so there is no word-misaligned stack
pointer for a guard to refuse. A check that cannot fire on any reachable input is not free.
It is a claim about the machine, and a reader will believe it.

Notice that the granularity matters and that getting it wrong is worse than omitting the
leg. Refusing a *word*-misaligned pointer is merely inert, for the reason above. Refusing a
*doubleword*-misaligned one would actively break the system: the procedure call standard
requires eight-byte alignment at public interfaces, not continuously, so a thread
interrupted immediately after pushing an odd number of registers is holding a perfectly
legal word-aligned stack pointer, and the guard would panic it. A leg whose failing input
is a legal state is not a strict check, it is a bug with a security-shaped comment above it.

So the fleet declines the leg and records why beside the code, and
`user/apps/common/pspguard/` writes a stack pointer with both low bits set and reads it
back, so the architectural claim is *observed* on the part rather than quoted from a manual.
Symmetry across a fleet is a strong default (it is the uniform-fleet thesis of Chapter 1),
and a leg that is provably inert on one member is one of the few places to break it
deliberately.

## The transferable rules

Six, in the order they cost to learn:

1. **Whoever writes the frame owns the check.** Hardware stacking is checked by hardware,
   at the pre-exception privilege, and only for the bytes the architecture defines. Every
   byte a kernel adds is outside that guarantee, and every byte of a fully software
   prologue is.
2. **A privileged prologue's wild store is an escalation primitive, not a crash.** The
   protection unit is bypassed at the privilege the prologue runs at, by whichever
   mechanism the ISA uses. And with no padding between allocations, the target need not be
   kernel memory: a neighbour's granted region is enough.
3. **A hazard you can state is not a hazard you have closed.** A comment explaining why one
   path must not trust the stack pointer is an argument about a mechanism, and it applies
   to every path that touches it.
4. **Check the extent, in the direction of travel, without arithmetic that can wrap.** A
   pointer-in-range test passes at the boundary and puts the whole frame outside the grant.
   Phrase the comparison as room remaining, and prove the subtraction's operands before you
   subtract.
5. **Bound everything that runs on the memory, not just the frame.** If kernel code
   continues on the caller's stack -- and there are good reasons to let it -- the bound is
   the frame plus the deepest descent, plus whatever the hardware can stack on top of that
   descent while it runs.
6. **Derive the margin, and make the derivation refuse when it is blind.** A number in a
   header rots. Re-derive it per build, keep the measured and the structural halves apart,
   assert it against the floor it has to fit under, and fail loudly on an unresolved
   indirect call, a reachable cycle or a dynamic frame rather than reporting a number you
   cannot justify.

The through-line is the one Chapter 3.9 states for syscall arguments and Chapter 7.4 states
for privilege: the kernel is on the wrong side of its own boundary, and the boundary is
wherever untrusted data is *used*, not wherever it was declared. A stack pointer is data. It
arrives earlier than anything else and it is used before any code has run, which is what
makes it the hardest instance of an otherwise familiar rule.

## Where to go next

- Why the kernel's dispatch runs on the caller's stack in the first place, and what that
  buys: Chapter 3.9,
  *[The syscall path: trap, dispatch, return](the-syscall-path-trap-dispatch-return.md)*.
- What "privileged" names, axis by axis, and why the memory-posture axis is the one that
  makes the wild store succeed: Chapter 7.4,
  *[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*.
- Why the memory below a stack belongs to a neighbour, and the allocator that makes it so:
  Chapter 3.6, *[Thread stacks and the KISS tension](thread-stacks-and-the-kiss-tension.md)*.
- The other half of never trusting a caller's pointer, on the ordinary syscall path:
  Chapter 7.1,
  *[Alignment across the syscall boundary](alignment-across-the-syscall-boundary.md)*.
- The same "commit it at the physical instant, not the intended one" discipline applied to
  the protection unit itself: Chapter 7.5,
  *[Protection follows the CPU, not the scheduler's intent](protection-follows-the-cpu-not-the-schedulers-intent.md)*.
- The RISC-V trap entry worked end to end, with the trusted trap stack in context:
  [`porting-a-new-isa-riscv.md`](porting-a-new-isa-riscv.md).
- The exact contracts: [`../reference/invariants.md`](../reference/invariants.md)
  (`fault-frame-untrusted-until-proved`, `syscall-in-priv-thread-context`,
  `mpu-bypass-windows-accepted`) and [`../reference/porting.md`](../reference/porting.md)
  (what a new backend must implement, and what it must not trust).
</content>
