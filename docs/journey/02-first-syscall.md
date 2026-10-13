<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 2. The first syscall: trap, dispatch, the handle resolve, return

This chapter follows a `kos::print` call from user code through the trap instruction and
kernel checks, then back to the caller with a result. You will see what Cortex-M hardware
saves when it takes a trap and what the kernel saves itself. You will also see why the kernel
leaves the trap handler almost at once, how it checks a user pointer before reading through
it, and how it resolves a handle (the number user code uses to name a kernel object) or
rejects it.

The ideas live in the Book: [The syscall path: trap, dispatch, return](../book/the-syscall-path-trap-dispatch-return.md)
is the main one, with [Interrupts and traps](../book/interrupts-and-traps.md) for what a trap
is, and [Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md) for handles. This
chapter shows the code. It assumes you know how to boot an image and attach gdb
([Setup](00-setup.md)) and that you have seen the first thread reach user mode
([Power-on](01-power-on.md)).

## The image and the syscall we follow

We use the `hello` app on the `qemu` build (`user/apps/common/hello/main.cc`). Its `main` prints
two lines, makes two semaphores, starts two threads called `ping` and `pong`, and parks. The very
first thing it does is:

```
    kos::print("hello from KickOS userspace!\n");
```

That print is the syscall we follow, for three reasons. It is the first one the app makes, so
nothing else has happened in that thread yet. Its effect is visible: the text appears on the
console. And it is two syscalls in a row that together touch every stop of this chapter. The
first is a `SEND` on handle 0, which carries a user pointer and a handle; in this image the
handle names nothing, so the kernel refuses it with an error code. The second is the kernel
console write, which carries a pointer and succeeds. Neither of them blocks, so the walk stays
in one thread; the blocking case is [the next chapter](03-sleep.md).

To run it under gdb (see [Setup](00-setup.md) for the details):

```
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/hello/hello -gdb tcp::1234 -S &
gdb-multiarch build/qemu/user/apps/common/hello/hello \
    -ex 'target remote :1234'
```

One gdb habit to have from the start: the app's `main` is compiled under another name,
`kickos_app_main` (`user/include/kickos/app.h` explains the rename), so `break main` finds
nothing. Use `break kickos_app_main`. And gdb reads kernel memory only while the core is stopped
in privileged code ([README](README.md#reading-with-gdb-or-without-it)): from `svc_trampoline`
onwards in this chapter.

### Stop 1: the app asks for a print
**Where:** `user/include/kickos/kos.h`, `kos::print`; `user/src/print.cc`, `kos_print`;
`user/include/kickos/sys/emit.h`, `kickos::stdout_write`

`kos::print` is a thin C++ wrapper over the C function `kos_print`, which measures the string and
hands it to `kickos::stdout_write`. That is the one function every way of writing to stdout ends
in: `kos_print`, the C library's `write`, and the test harness.

`stdout_write` does not go to the kernel console first. It sends the bytes as a message on handle
0 of the calling thread, which by convention is the thread's stdout endpoint
(`KOS_CAP_STDOUT` in `system/include/kickos/sys/cap_index.h`). An endpoint is a kernel object two
threads use to pass messages; when a console driver runs in userspace, it receives on the other
side. Only if the send is refused for one of three "nobody is serving" reasons does it fall back
to the kernel's own debug console:

```
        int32_t const r = kos_send(0, s + sent, chunk);
        // ...
        if (r >= 0)
        {
            sent += static_cast<size_t>(r);
            continue;
        }
        if (r != -KOS_EBADF and r != -KOS_EAGAIN and r != -KOS_ECONNREFUSED)
        {
            return {sent, r};
        }
        // ...
        WriteResult const k = kconsole_offer(s + sent, total - sent);
```

`hello` on `qemu` runs no console driver, so handle 0 is empty. Keep that in mind: it is what makes
the first syscall fail at Stop 8.

**Try it:** `break kos_send`, `continue`, then `bt`. You stop in the stub, called from the print:

```
#0  kos_send (ep=0, buf=0xf243, len=29) at .../user/src/syscall_stubs.cc:113
#1  0x00001930 in kickos::stdout_write (s=0xf243 "hello from KickOS userspace!\n", total=29) at .../emit.h:88
#2  kos_print (s=0xf243 "hello from KickOS userspace!\n") at .../user/src/print.cc:11
#3  0x0000019a in kos::print (...) at .../user/include/kickos/kos.h:17
#4  kickos_app_main () at .../user/apps/common/hello/main.cc:58
```

`buf=0xf243` is an address in flash, where the string literal lives. `p $control` prints `3`:
bit 0 (`nPRIV`) set means unprivileged, bit 1 (`SPSEL`) set means the thread runs on the process
stack pointer, `PSP`. This is user mode.

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
(section "The surface is deliberately small", on why the console write is the one kernel I/O
path), [Reference: console](../reference/console.md)

### Stop 2: the stub puts the request in registers
**Where:** `user/src/syscall_stubs.cc`, `kos_send`; `arch/arm/armv7m/switch.S`, `arch_syscall`

Every syscall stub is the same shape: cast the arguments to plain words and call `arch_syscall`
with the syscall number first. The number for a send is `KOS_SYS_SEND`, which is 27, from the
`kos_syscall_nr` enum in `user/include/kickos/sys/abi.h`. Those numbers are the ABI between a
compiled app and the kernel and are never renumbered.

```
int32_t kos_send(kos_cap_t ep, void const* buf, size_t len)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_SEND,
                                             static_cast<uintptr_t>(ep),
                                             reinterpret_cast<uintptr_t>(buf),
                                             static_cast<uintptr_t>(len), 0));
}
```

`arch_syscall` takes five words. The ARM calling convention (AAPCS) passes the first four in
`r0`-`r3` and the fifth on the stack, so on entry `r0` is the number, `r1` the handle, `r2` the
buffer, `r3` the length, and the fifth word sits at `[sp]`. The stub's whole body is three
instructions:

```
arch_syscall64:
    .thumb_func
    .global arch_syscall
arch_syscall:
    ldr     r12, [sp]               /* a3 (5th arg) -> stacked r12 */
    svc     #0
    bx      lr
```

The `ldr` moves the fifth argument into `r12`, because `r12` is one of the registers the hardware
will save on the trap, and the stack slot is not. Notice that `arch_syscall64` is the same code:
a 64-bit result comes back in the pair `r0:r1`, and the narrow version simply ignores `r1`.

Nothing here is copied into a buffer. All five words travel in registers, so there is nothing
in memory the caller could change while the kernel reads it.

**Try it:** `break arch_syscall`, `continue`, `info registers r0 r1 r2 r3`:

```
r0             0x1b                27
r1             0x0                 0
r2             0xf243              62019
r3             0x1d                29
```

`x/2wx $sp` shows `0x00000000` first: the fifth argument, 0.

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("The number and the arguments: a register ABI"), [Reference: invariants](../reference/invariants.md)
(`syscall-return-abi`)

### Stop 3: the trap, and what the hardware saves
**Where:** `arch/arm/armv7m/switch.S`, the `svc #0` in `arch_syscall`, then `SVC_Handler`

`svc` is the Cortex-M trap instruction ("supervisor call"). It raises exception number 11,
SVCall, synchronously: the CPU stops the thread at that instruction and enters the handler the
vector table names for SVCall, which is `SVC_Handler`. On entry the hardware does four things on
its own, before any kernel code runs:

1. It pushes eight words onto the stack the thread was using (here the `PSP`): `r0`, `r1`, `r2`,
   `r3`, `r12`, `lr`, the return address `pc`, and the status register `xPSR`. This is the
   *hardware exception frame*. Those are the registers the C calling convention lets a function
   destroy, so this is exactly the set a C handler could clobber.
2. It switches to *handler mode*, which is always privileged, and to the main stack pointer, `MSP`.
3. It writes a magic value into `lr`, called `EXC_RETURN`. Branching to it later (`bx lr`) means
   "return from the exception": the hardware pops the eight words back and resumes the thread.
4. It records the exception number in `IPSR`, the low bits of `xPSR`.

What the hardware does **not** save is `r4`-`r11`. The C calling convention says a called
function must preserve those, so a handler written in C leaves them intact. Software saves them
only when it is about to run a *different* thread on this core, which is the job of
`PendSV_Handler`
([01, Stop 13](01-power-on.md#stop-13-the-first-pendsv-switches-to-root-and-turns-the-mpu-on)).
The comment at the top of `switch.S` draws the full picture of a switched-out thread's stack:
`{r4-r11, EXC_RETURN}` pushed by software below the hardware's `{r0-r3, r12, lr, pc, xPSR}`.

**Try it:** from the `arch_syscall` stop, `stepi` twice. You are on the first instruction of
`SVC_Handler`. Then:

```
(gdb) info registers msp psp lr
msp            0x203fff60
psp            0x20027ed8
lr             0xfffffffd
(gdb) p/x $xpsr
$2 = 0x100000b
(gdb) x/8wx $psp
0x20027ed8:    0x0000001b    0x00000000    0x0000f243    0x0000001d
0x20027ee8:    0x00000000    0x000016c7    0x000024b8    0x01000000
```

Read the frame left to right: `r0`=27 (the number), `r1`=0 (the handle), `r2`=0xf243 (the
buffer), `r3`=29 (the length), `r12`=0 (the fifth argument), `lr`=0x16c7 (where `kos_send`
continues after its call to `arch_syscall`, with bit 0 set for Thumb), `pc`=0x24b8 (the `bx lr`
right after the `svc`), `xPSR`=0x01000000 (Thumb bit, thread mode). The `PSP` moved down by 32
bytes, one frame. `xPSR` ends in `0xb`, exception 11, and `EXC_RETURN` 0xfffffffd means "return to
thread mode, on the `PSP`, with no floating-point state in the frame".

**Why, and the contract:** [Book: interrupts and traps](../book/interrupts-and-traps.md),
[Book: context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md)

### Stop 4: the handler checks the stack, then gets out
**Where:** `arch/arm/armv7m/switch.S`, `SVC_Handler`, the `PSP_GUARD` macro, the `.Lsvc_slow` label

You might expect the handler to look at the number and call the right kernel function. It does
not, and the reason is the rule this whole path is built on: **the kernel's dispatch must run in
privileged thread mode, on the caller's own continuation, never in the handler**
(`syscall-in-priv-thread-context`). The *continuation* is the rest of the caller's work: its own
stack and the chain of calls on it, which carry on where the trap left them. A thread that blocks
inside a syscall has to really stop before it returns to user code. On Cortex-M a switch requested from handler mode is only
*pended* and happens after the handler returns, so a blocking call made inside the handler would
return to the caller first and switch away later. The Book chapter spells out this trap.

So the handler does three small things. First it reads the `PSP` and checks it with `PSP_GUARD`:
the frame the hardware just wrote, and everything the kernel will push next, must fit inside the
calling thread's own stack bounds (`stack_lo`, `stack_hi` in `struct arch_context`). The `PSP` is a
value the user thread chose, and the handler is about to keep writing through it with privilege,
so it is checked before anything else (Book: [Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md)).

Second it compares the stacked `r0` with 56. That is `KOS_SYS_CALL_REG`, the one syscall a
Cortex-M build may finish inside the handler, an IPC fast path covered in
[Call and reply](05-call-and-reply.md). Ours is 27, so it branches to the ordinary path.

Third, the ordinary path, which is the heart of the trick:

```
.Lsvc_slow:
    ldr     r1, =svc_trampoline
    bic     r1, r1, #1              /* stacked PC must be halfword-aligned (bit0=0;
                                       the T-bit comes from the stacked xPSR) */
    str     r1, [r0, #24]           /* frame.PC := svc_trampoline (offset 6*4) */
    mrs     r1, control
    bic     r1, r1, #1              /* CONTROL.nPRIV = 0 -> trampoline privileged */
    msr     control, r1
    isb
    bx      lr                      /* exception-return into svc_trampoline:
                                       r0=nr r1=a0 r2=a1 r3=a2 r12=a3
                                       LR=arch_syscall caller-return */
```

It overwrites the saved `pc` in the frame (word 6, offset 24) with the address of
`svc_trampoline`, clears `nPRIV` so thread mode becomes privileged, and returns from the
exception. The hardware pops the frame as usual: every argument register comes back exactly as
the caller left it, but execution resumes at `svc_trampoline` instead of after the `svc`, in
thread mode, privileged, on the caller's own `PSP`. Nothing was copied. The handler never read a
kernel data structure.

**Try it:** `break svc_trampoline`, `continue`, then:

```
(gdb) info registers r0 r1 r2 r3 r12 lr sp
r0             0x1b                27
r1             0x0                 0
r2             0xf243              62019
r3             0x1d                29
r12            0x0                 0
lr             0x16c7              5831
sp             0x20027ef8          0x20027ef8
(gdb) p $control
$1 = 2
```

The registers are the caller's, `sp` is back where it was before the trap (the frame was popped),
and `CONTROL` is 2: still on the `PSP`, but privileged now.

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("The contract: dispatch runs in privileged THREAD context", "Per-arch entry"),
[Reference: invariants](../reference/invariants.md) (`syscall-in-priv-thread-context`,
`fault-frame-untrusted-until-proved`)

### Stop 5: the trampoline moves onto the kernel stack
**Where:** `arch/arm/armv7m/switch.S`, `svc_trampoline`; `arch/arm/armv7m/include/kickos/arch/context.h`, `struct arch_context`

On this build each thread owns a small *kernel stack*, a block in kernel memory that only
privileged code can touch (`KICKOS_KERNEL_STACKS` is 1 on `qemu`, as on any armv7m chip with an
MPU). The banner at boot says how big: `kstack 1008 B x 17`. The trampoline switches `sp` onto that
block before it calls any C, so the kernel's own stack frames never sit in memory the user thread
can read or write.

```
    push    {r0, r1}                /* the two scratch registers the loads below need */
    ldr     r0, =g_arch_current
    ldr     r0, [r0]
    ldr     r0, [r0, #F_CTX_KERNEL_SP]
    // ...
    add     r1, sp, #8              /* the PSP the thread resumes on: the push undone */
    str     r1, [r0, #-8]
    str     lr, [r0, #-12]
    str     r12, [r0, #-16]         /* a3 into the fifth-argument slot */
    sub     r12, r0, #16            /* r12 is free once a3 is in the header */
    pop     {r0, r1}
    mov     sp, r12                 /* on the calling thread's kernel block from here */
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0:r1 */
```

`g_arch_current` points at the running thread's `struct arch_context`, and its `kernel_sp` field
is the top of that thread's kernel block. Just below the top the trampoline writes a four-word
header: the fifth argument (where the C calling convention expects it, at `[sp]`), the address to
return to, and the user `PSP` to come back to. Then it calls `syscall_dispatch` with an ordinary
`bl`. From here on the kernel is plain C++.

**Try it:** `break syscall_dispatch`, `continue`:

```
Breakpoint 3, syscall_dispatch (nr=27, a0=0, a1=62019, a2=29, a3=0) at .../kernel/syscall/syscall.cc:371
(gdb) info registers sp
sp             0x20000830          0x20000830 <kickos::(anonymous namespace)::g_kstacks+2000>
(gdb) x/4wx $sp
0x20000830 <...g_kstacks+2000>:    0x00000000    0x000016c7    0x20027ef8    0x4b53464c
```

`sp` is now inside `g_kstacks`, the array of kernel blocks. The header reads: fifth argument 0,
return address 0x16c7, user `PSP` 0x20027ef8, and an alignment pad. `bt` shows
`syscall_dispatch` called from `svc_trampoline`, and nothing above it: the user frames are on
another stack. While you are stopped privileged, `p/x *(struct arch_context*)*(unsigned*)&g_arch_current`
prints the running thread's context, `kernel_sp = 0x20000840` among it.

**Why, and the contract:** [Book: whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md)
(why a per-thread kernel stack), [Reference: architecture](../reference/architecture.md)
("User/kernel separation")

### Stop 6: one door, one switch
**Where:** `kernel/syscall/syscall.cc`, `syscall_dispatch` and `syscall_body`

`syscall_dispatch` is the only entry into the kernel from user code. It first finds the caller,
`sched::current()`, and checks one thing about it: if another thread has asked for this one to be
cancelled, its next syscall is where it dies (`sched::exit_current`). Then it calls
`syscall_body`, which is a single flat `switch` on the number. Each case checks its own arguments
at the top and then calls the kernel function that does the work. An unknown number falls to the
`default`, which answers `-KOS_EINVAL`; a value from user code must never be able to stop the
kernel.

```
        case KOS_SYS_SEND:
        case KOS_SYS_SEND_TIMED:
        {
            // No dispatch IrqLock: endpoint_send takes and releases its own around the
            // park, and a spanning caller lock would livelock ARM.
            uint32_t timeout = KOS_TIMEOUT_NONE;
            if (nr == KOS_SYS_SEND_TIMED)
            {
                timeout = static_cast<uint32_t>(a3);
            }
            return static_cast<uint64_t>(endpoint_send(static_cast<uint32_t>(a0), a1,
                                                       static_cast<size_t>(a2), timeout));
        }
```

The arguments arrive as `uintptr_t` words and each case casts them to what they mean. The return
type is `uint64_t` on every target, so one dispatcher serves the syscalls that return 64-bit
values (the clock read) and the ones that return a small signed count.

`IrqLock` is the kernel's critical section: on Cortex-M it raises `BASEPRI` so that interrupts
which could touch kernel state wait until it is released. You will see one in almost every case.

**Try it:** at the `syscall_dispatch` stop, `info args` prints `nr = 27, a0 = 0, a1 = 62019, a2 = 29,
a3 = 0`: the same five words, now C++ parameters.

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("Validate, then use"), [Reference: invariants](../reference/invariants.md)
(`user-args-validated-at-boundary`)

### Stop 7: checking the caller's pointer
**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_send`; `kernel/syscall/syscall_mem.cc`,
`user_readable_ok` and `user_range_ok`

Here is the danger. `syscall_dispatch` runs privileged, and on this MPU the privileged level is
allowed to read and write almost everything. So the memory protection that stops the user thread
from reading kernel memory does **not** stop the kernel from reading kernel memory on the user's
behalf. If the kernel trusted `buf`, a user could pass the address of another task's secret and
have the kernel copy it out. Every pointer a syscall receives is checked against what the
*caller* is allowed to touch, before the kernel reads a byte.

`endpoint_send` checks the length and the pointer first, before it looks at anything else:

```
    int32_t endpoint_send(uint32_t cap, uintptr_t buf, size_t len, uint32_t timeout_us)
    {
        if (len > KOS_EP_MSG_MAX)
        {
            return -KOS_EINVAL;
        }
        if (not user_readable_ok(buf, len))
        {
            return -KOS_EFAULT; // Validate in caller context.
        }
```

`user_readable_ok` asks `user_range_ok`, which walks the caller's own list of memory regions
(`Thread::mpu`, the same list the MPU is programmed from when the thread runs) and accepts the
range only if one region with read permission holds all of it. It rejects a range that wraps
past the top of the address space, and for a write it also rejects a range that overlaps any
read-only region. The pointer is not dereferenced here at all; this is pure arithmetic on the
region table.

**Try it:** stop at `kos_send` first (the root task makes its own checks earlier in boot), then
`break kickos::user_readable_ok` and `continue`:

```
Breakpoint 2, kickos::user_readable_ok (ptr=62019, len=29) at .../kernel/syscall/syscall_mem.cc:364
(gdb) p kickos::sched::current()->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p kickos::sched::current()->mpu.regions_[0]
$3 = {base = 0, size = 4194304, attr = 5}
(gdb) p kickos::sched::current()->mpu.regions_[1]
$4 = {base = 536936448, size = 65536, attr = 3}
```

Region 0 is the 4 MiB of flash with `attr = 5`, which is read plus execute (`ARCH_MPU_R` is 1,
`ARCH_MPU_W` 2, `ARCH_MPU_X` 4, in `arch/include/kickos/arch/arch.h`). The string at 0xf243 lies
inside it, so `finish` prints `Value returned is ... = true`.

**Why, and the contract:** [Book: alignment across the syscall boundary](../book/alignment-across-the-syscall-boundary.md)
(null, alignment and ownership, in that order), [Reference: invariants](../reference/invariants.md)
(`user-args-validated-at-boundary`)

### Stop 8: resolving the handle
**Where:** `kernel/syscall/cap.cc`, `cap_resolve_e` and `cap_lookup`

The first argument of a send is a *handle*: the number that picks a slot in the calling thread's
capability table, a per-thread array of `CapEntry` slots that each name one kernel object and the
rights this thread has over it. Such a slot is a *capability*. User code never sees a kernel pointer. Every syscall that names an object
turns the handle into the object through one function, `cap_resolve_e`, before it touches the
object, links a queue, or changes a counter. One function means one place to audit.

```
    CapEntry* cap_lookup(Thread* c, uint32_t cap_handle)
    {
        // ...
        uint32_t const idx = cap_handle & KCAP_INDEX_MASK;
        // ...
        if (idx >= thread_cap_capacity(c))
        {
            return nullptr; // capacity 0 (no run) is caught here too: nothing is in range
        }
        CapEntry& e = *cap_slot(c->caps, idx);
        if (e.type == static_cast<uint8_t>(CapType::CAP_EMPTY))
        {
            return nullptr;
        }
        uint32_t const cgen = cap_handle >> KCAP_INDEX_BITS;
        if (static_cast<uint32_t>(e.gen) != cgen)
        {
            return nullptr;
        }
        return &e;
    }
```

A handle is two numbers in one word: the low 16 bits are the slot index, the high bits a
*generation*. Each time a capability is closed and its slot emptied (`cap_slot_vacate`), the
slot's generation goes up by one, so an old handle to a closed object stops matching instead of
silently naming the slot's next occupant
([A stale handle must not be usable](../book/stale-handles-generation-width-and-allocation-policy.md)).
`cap_lookup` checks the index against the table size, refuses an empty slot, and compares the
generation. `cap_resolve_e` then checks the slot's type against what the syscall wants (an
endpoint here), checks the rights (a send needs `CAP_SIGNAL`, value 2), and finally asks the
object pool, which checks a second generation on the object itself. Any failure sets the error
and returns null: `-KOS_EBADF` for "names nothing usable", `-KOS_EACCES` for "names the right
thing but you may not do that with it". The caller in `endpoint_send` is inside an `IrqLock`, so
nobody can close the object between the resolve and its use.

For `hello`'s `main`, slot 0 has never been filled.

**Try it:** `break kickos::cap_resolve_e if cap_handle == 0`, `continue`:

```
Breakpoint 1, kickos::cap_resolve_e (c=c@entry=0x20004e90 <...>, cap_handle=cap_handle@entry=0,
    want=want@entry=kickos::CapType::CAP_ENDPOINT, need=need@entry=2 '\002', err=...) at .../kernel/syscall/cap.cc:1019
(gdb) p c->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p c->caps.chunk[0][0]
$3 = {obj = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0}
(gdb) finish
Value returned is $4 = (void *) 0x0
```

`type = 0` is `CapType::CAP_EMPTY`. To see a resolve succeed, look at the `ping` thread a little
later: `delete`, `break kos_sem_post`, `continue` (it stops with `sem=3`), then
`break kickos::cap_resolve_e`, `continue`:

```
(gdb) p c->name
$5 = "ping", '\000' <repeats 11 times>
(gdb) p c->caps.chunk[0][cap_handle]
$6 = {obj = 1, type = 1 '\001', seq_lo = 0 '\000', rights = 7 '\a', seq_hi = 0 '\000', gen = 0}
(gdb) finish
Value returned is $7 = (void *) 0x200048f4 <kickos::detail::g_instance+324>
(gdb) p *(kickos::Semaphore*)$
$8 = {count = 0, waiters = {head = 0x20005148 <kickos::detail::g_instance+2456>, tail = 0x20005148 <...>}}
```

Slot 3 holds a semaphore (`type = 1`, `CAP_SEM`) with all three rights (wait, signal, transfer).
The resolve returns the kernel's `Semaphore`, and its wait list already holds one thread: `pong`,
waiting for this very post.

**Why, and the contract:** [Book: naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Reference: architecture](../reference/architecture.md) ("Object model, capabilities & IPC")

### Stop 9: an error comes home
**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_send`; `system/include/kickos/sys/errno.h`

Back in `endpoint_send`, the null pointer turns into a return of `-err`, that is `-KOS_EBADF`. The
convention for every syscall that can fail is the same: success is zero or a positive count,
failure is a *negated* error code, so `rc < 0` always means an error and can never be confused
with a byte count (`syscall-return-abi`). The codes follow POSIX `errno` values where one exists:
`KOS_EBADF` is 9, as in POSIX. The Book lists the few syscalls that cannot use this convention,
such as one returning a pointer, and why.

The `-9` travels back up the same path you will see in Stop 11, and `stdout_write` reads it as
"nobody serves my stdout", which is one of its three fallback reasons.

**Try it:** `break kos_send`, `continue`, `finish`:

```
kickos::stdout_write (s=0xf243 "hello from KickOS userspace!\n", total=29) at .../emit.h:92
92            if (r >= 0)
Value returned is $1 = -9
```

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("The return convention: a negated errno, and where it cannot reach"),
[Reference: invariants](../reference/invariants.md) (`syscall-return-abi`)

### Stop 10: the second syscall copies through the kernel
**Where:** `kernel/syscall/syscall.cc`, the `KOS_SYS_KCONSOLE_WRITE` case and `console_write_user`;
`kernel/syscall/syscall_mem.cc`, `kaccess_from_user`; `kernel/init/console.cc`, `kconsole_write_user`

`kconsole_offer` calls `kos_kconsole_write`, and the same trap path runs again with number 1,
`KOS_SYS_KCONSOLE_WRITE`. Its case is short, and every line is a check:

```
        case KOS_SYS_KCONSOLE_WRITE:
        {
            // The kernel reads buf privileged, so an unbounded buffer would launder another
            // domain's arena page out through the console.
            constexpr size_t MAX_CONSOLE_WRITE = 4096;
            size_t len = static_cast<size_t>(a1);
            if (len > MAX_CONSOLE_WRITE)
            {
                len = MAX_CONSOLE_WRITE;
            }
            if (not user_readable_ok(a0, len))
            {
                // NOT 0: a len-0 write legitimately returns 0.
                return static_cast<uint64_t>(-KOS_EFAULT);
            }
            return static_cast<uint64_t>(console_write_user(a0, len));
        }
```

The length is clamped (a scalar check), the range is checked as in Stop 7 (a pointer check), and
only then does `console_write_user` read the bytes. It does not read them in place: it copies
them in chunks into a small buffer on the kernel stack with `kaccess_from_user`, and hands that
kernel copy to `kconsole_write_user`, the kernel console's writer (`console_emit` takes it from
there to the chip's console device). The comment on
the `-KOS_EFAULT` is worth reading twice: a zero-length write is a real success that returns 0, so
0 cannot also mean "refused".

`kconsole_write_user` can make the caller wait when the console has no room. That is a blocking syscall,
and it works exactly like the sleep in the next chapter.

**Try it:** `break kos_kconsole_write`, `continue`, then `break kickos::kaccess_from_user`,
`continue`, `bt 4`:

```
#0  kickos::kaccess_from_user (kdst=kdst@entry=0x20000768 <...g_kstacks+1800>, sspace=sspace@entry=0x0,
    usrc=usrc@entry=62019, n=n@entry=29) at .../kernel/syscall/syscall_mem.cc:643
#1  0x00007b24 in kickos::(anonymous namespace)::console_write_user (buf=buf@entry=62019, len=len@entry=29) at .../syscall.cc:84
#2  0x00007cac in (anonymous namespace)::syscall_body (nr=1, a0=62019, ...) at .../syscall.cc:432
#3  syscall_dispatch (nr=1, a0=62019, a1=<optimized out>, a2=0, a3=0) at .../syscall.cc:400
```

`kdst` is inside `g_kstacks`: the chunk buffer lives on `main`'s kernel stack.
`sspace=0x0` means "a plain address in the one shared address space": on an MPU board there is
no translation, so the copy reads the user address directly once the range has been checked. On
an MMU board the same call walks the caller's page tables ([The MMU](10-the-mmu.md)).

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("The surface is deliberately small", on the console write), [Reference: console](../reference/console.md)

### Stop 11: the way back
**Where:** `arch/arm/armv7m/switch.S`, the end of `svc_trampoline`

`syscall_dispatch` returns its `uint64_t` in `r0:r1`, and the trampoline undoes what it did on the
way in, in a careful order:

```
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0:r1 */
    ldr     lr, [sp, #4]            /* the caller-return */
    ldr     r2, [sp, #8]            /* the resume PSP, read before sp leaves this block */
    mov     sp, r2
    // ...
    ldr     r2, =g_arch_current
    ldr     r2, [r2]                /* current ctx* */
    ldr     r2, [r2, #8]            /* ctx.resting_npriv (offset 8) */
    mrs     r3, control
    bfi     r3, r2, #0, #1          /* CONTROL.nPRIV = resting privilege */
    msr     control, r3
    isb
    bx      lr                      /* -> arch_syscall's caller, r0:r1=result */
```

It reloads the return address and the user `PSP` from the header, moves `sp` back to the user
stack, and only then drops privilege, because the header is kernel memory that unprivileged code
may not read. It drops to the thread's *resting* privilege, `resting_npriv`, not to a hard-coded
"unprivileged": a privileged thread that makes a syscall stays privileged
(`syscall-restores-resting-priv`). Lowering privilege is a plain register write; raising it took a
trap. Then `bx lr` returns straight into `kos_kconsole_write`, skipping the `bx lr` in
`arch_syscall`, which only runs if something unusual returns there.

No exception return happens on the way out. The exception ended at Stop 4; this is an ordinary
function return in thread mode.

**Try it:** the last `bx lr` has no symbol of its own. `disassemble svc_trampoline` and find the
`bx lr` just before the `cpsid i`; in this build it is at 0x247c. While stopped inside the console
write (Stop 10), `delete`, `break *0x247c`, `continue`, then:

```
(gdb) info registers r0 r1 lr sp
r0             0x1d                29
r1             0x0                 0
lr             0x1615              5653
sp             0x20027ef8          0x20027ef8
(gdb) p $control
$2 = 3
(gdb) stepi
0x00001614 in kos_kconsole_write (buf=<optimized out>, len=<optimized out>) at .../user/src/syscall_stubs.cc:35
```

The result is 29, every byte written. `CONTROL` is 3 again before the `bx`: unprivileged, on the
`PSP`, back in user code. The text is on the console.

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("The way back: resting privilege, atomically"), [Reference: invariants](../reference/invariants.md)
(`syscall-restores-resting-priv`, `trap-return-atomic`)

### Stop 12: when the syscall blocks instead
**Where:** `arch/arm/armv7m/switch.S`, `PendSV_Handler`; `kernel/sync/sync.cc`, `wq_block`

Both syscalls above returned straight away. Many do not: `main` itself ends in `idle.wait()`, a
semaphore wait nobody ever posts, and `ping` sleeps 0.4 s on every round. Nothing in Stops 1-11
changes for them. Because the dispatch runs in thread mode on the thread's own kernel stack, a
blocking call is just C++ code that decides to park the thread and asks the scheduler for another
one. The other thread runs; the half-finished syscall stays frozen on the parked thread's kernel
stack, below the header the trampoline wrote; and when the thread is woken it resumes inside the
kernel function that parked it, returns into `syscall_body`, and takes the same way back as
Stop 11. There is no separate "continuation" object: the call stack is the continuation.

[The next chapter](03-sleep.md) follows exactly that, with a sleep.

**Try it:** `break kos_sleep_ns`, `continue`, then `break PendSV_Handler`, `continue`, `info
registers psp`. The `PSP` reads 0x20000bb8, an address inside `g_kstacks`, not a user stack: the
thread being switched out was parked in the middle of its syscall.

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("Blocking syscalls are ordinary context switches"), [Book: the blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)

## On the silicon twin

`f411disco` runs the same `arch/arm/armv7m/switch.S` and the same kernel; its STM32F411 has an
MPU, so it also gets per-thread kernel stacks. The one difference on this walk is where the
console bytes end up: on `qemu` the chip file `arch/arm/chip/mps2/chip_mps2.cc` hands them to QEMU
through semihosting, and on the F411 `arch/arm/chip/stm32f411/chip_stm32f411.cc` drives USART2.

## Where to next

- [3. Sleep](03-sleep.md): a syscall that blocks, the timer, and the wake.
- [4. Mutex](04-mutex.md): a contended object, where the handle resolve and the park meet.
- [5. Call and reply](05-call-and-reply.md): the `KOS_SYS_CALL_REG` fast path that `SVC_Handler`
  tests for.
- [8. Spawn and capabilities](08-spawn-and-caps.md): how slot 3 of `ping`'s table got its semaphore.
- [9. Fault and kill](09-fault-and-kill.md): what happens when the `PSP_GUARD` check fails.
- [14. RISC-V](14-riscv.md): the same stops through `ecall` and a different trap entry;
  [13. ARMv8-M](13-armv8m.md) shows this chapter's trap running unchanged on a Cortex-M33.

## Exercises

1. `main`'s capability table has 11 slots (`p c->cap_width` at a `cap_resolve_e` stop). Predict
   what `kos_send(40, "x", 1)` from `main` returns and which test in `cap_lookup` refuses it. Then
   predict the answer for `(1u << 16) | 3`, which names slot 3 with the wrong generation, once
   `main` has created its semaphores. Check both with `break kickos::cap_resolve_e if cap_handle == 40`
   (and the second value) and `finish`.
2. In a scratch app, write to a kernel address through the public fallback:
   `kickos::kconsole_offer(reinterpret_cast<char const*>(0x20004000), 4)` (the raw
   `kos_kconsole_write` is poisoned in app code). Predict the `error` field of the result and the
   function that decides it. In gdb, `finish` out of `kickos::user_readable_ok` and read the answer.
3. At the `svc_trampoline` stop, `x/8wx $sp-32` shows the words the hardware frame occupied a moment
   ago. Which one now holds `svc_trampoline`'s address (0x2438 in this build), and why does the
   word before it still point into `kos_send`?
