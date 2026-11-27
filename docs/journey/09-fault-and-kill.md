<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 9. Fault and kill: a user fault, the report, the kill, who survives

By the end of this chapter you will have followed an unprivileged thread that executes an
undefined instruction, from the moment the Cortex-M core takes the exception to the moment a
different thread prints that it survived. You will see the hardware's own work (the stacked
frame, the fault status registers), the tests the kernel makes before it decides the fault is
the thread's and not its own, why the fault handler prints nothing, how the exception return is
bent so that the dying thread walks into its own exit, and what that exit releases:
capabilities, mutexes, waiters, the task. You will see how other threads learn about the
death and how the system continues. A short final section follows a fault the kernel does not
contain and shows the kernel's panic path.

The ideas live in the Book: [Interrupts and traps](../book/interrupts-and-traps.md) for what
an exception is, [Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md)
for why "unprivileged" is the line between a kill and a panic, and
[Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md)
for why a fault frame on a user stack is not trusted by default. The exact rules are the
[Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond)
and the `fault-*` entries of [Reference: invariants](../reference/invariants.md).

## The image we follow

We use `faultsurvive` (`user/apps/common/faultsurvive/main.cc`), built with
`KICKOS_FS_MODE=0`. Its composition declares two tasks: `main`, and `faulter`, a program of its
own (`user/apps/common/faultsurvive/peer.cc`) that starts only when `main` asks. `main` creates
the `faulter` task, waits until it is dead, and releases it. `faulter` prints one line and
executes `KOS_TRAP_ILLEGAL()`, which on Arm is the instruction `udf #0`, permanently undefined.
`main` must then print that it survived:

```
[fs] spawning the faulter
[fs] worker about to fault

=== THREAD FAULT === thread 'faulter' killed, system continues
  PC=0x13e80 CFSR=0x10000
[fs] survivor ran after the fault
```

QEMU then exits with status 0, the status `main` returned. The test that runs it is
`tests/integration/check_faultsurvive.sh`, which checks that the survivor line comes after the
kill line.

Start it halted and attach, as in [Setup](00-setup.md):

```sh
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/faultsurvive/faultsurvive -gdb tcp::1234 -S
gdb-multiarch build/qemu/user/apps/common/faultsurvive/faultsurvive -ex 'target remote :1234'
```

At your first stop in kernel code, set the two convenience variables of
[Spawn and capabilities](08-spawn-and-caps.md):

```
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) set $P = &kickos::kickos_kernel_pools
```

More images appear later: `faultsurvive_ovf` and `pspguard_below` (Stop 3), `selftest`
(Stop 7), `fault` (Stop 9) and `panicgate1` (the last section).

### Stop 1: the victim gets a task of its own
**Where:** `user/apps/common/faultsurvive/main.cc`, `main`;
`user/apps/common/include/kickos/apps/peer_run.h`, `create_peer`, `wait_dead`;
`kernel/syscall/syscall_thread.cc`, `task_create_call`

The unit a fault kills is the **task**, not the thread. The threads of one task share one
memory domain, so when one of them faults while writing, the others may be looking at
memory it left half-written; the kernel ends them all. A plain spawn puts the new thread in
the spawner's own task. So a program that wants to outlive a thread's fault must run that
thread in a task of its own. Here the faulter is a declared program, and `main` starts it with
`kos_task_create`:

```
    kos_program_t prog = KOS_CAP_NONE;
    if (kos_program_find(KICKOS_FS_PEER, &prog) != 0)
    {
        kos::print("[fs] ERROR: no faulter program\n");
        return 1;
    }
    // ...
        char const* const argv[] = {"faulter", arg_text, nullptr};
        kos_task_params params = {};
        params.argv = argv;
        // ...
        kos::print("[fs] spawning the faulter\n");
        kos_task_t victim = KOS_TASK_NONE;
        rc = kickos::apps::create_peer(prog, params, &victim);
        // ...
        int status = 0;
        do
        {
            rc = kickos::apps::wait_dead(victim, &status, std::chrono::seconds{1});
        } while (rc == -KOS_ETIMEDOUT);
        (void)kos_task_slay(victim);
```

`create_peer` is `kos_task_create`, retried through a `KOS_WATCH_INSTANCE` watch while the
create answers `-KOS_EAGAIN` for an instance whose last incarnation is still dying.
`wait_dead` reads `kos_task_state` and sleeps a millisecond between reads until the task is
`DEAD`. [Spawn and capabilities](08-spawn-and-caps.md) walked the create. Here, notice only
that `main` is the task's creator, which is what lets it read the task's state and release it
afterwards.

**Try it:** the second `task_create_call` is `main` starting `faulter` (the first is the init
starting `main`).

```
(gdb) break kickos::sched::start
(gdb) continue
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) set $P = &kickos::kickos_kernel_pools
(gdb) break kickos::task_create_call if $_streq($k->currents.cells[0]->name, "main")
(gdb) continue
(gdb) p ((kos_task_params*)pu)->argv[0]
$1 = 0x13799 "faulter"
(gdb) set $o = (unsigned*)out
(gdb) finish
Value returned is $2 = 0
(gdb) p/x *$o
$3 = 0x3
(gdb) p/x $P->tasks[2]
$4 = {domain = 0x20003294, free_node = {next = 0x0, prev = 0x0}, members = {head = 0x20002ca0}, refcount = 0x1, creator_tag = 0x2, gen = 0x0, prio_ceiling = 0xa, marks = 0x80, watch_bit = 0x0, sweeping = 0x0, image = 0x2, watch_notify = 0x0, watch_ep = 0x0, exit_status = 0x82, reent_seat = 0x20018000}
```

Task handle 3 is `tasks[2]`: one member, its ceiling the declaration's 10, created by `main`
(kill tag 2), started (`0x80`), and no watch.

**Why, and the contract:** [Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond)
("The rule"), [Reference: invariants, `fault-kills-the-unprivileged-thread-task`](../reference/invariants.md)

### Stop 2: the core takes the exception
**Where:** `arch/arm/common/core_vectors.inc`, `KICKOS_CORE_VECTORS_V7M`; `arch/arm/chip/mps2/startup.S`
(on the f411, `arch/arm/chip/stm32f411/startup.S`); `arch/arm/armv7m/arch_armv7m.cc`, `HardFault_Handler`

`faulter` runs unprivileged, in thread mode, on its own stack (the process stack pointer,
PSP). It executes `udf #0`. The core cannot execute it, so it raises a **UsageFault**. The
core bring-up (`kickos_armv7m_init` in `arch/arm/armv7m/arch_armv7m.cc`) enables UsageFault and
BusFault as exceptions of their own, and the PMSAv7 backend
(`arch/arm/common/arch_arm_mpu_pmsav7.cc`) enables MemManage, all in `SCB_SHCSR`. Had one of
them been left disabled, the fault would **escalate** to HardFault, and the core would record
that it did (HFSR.FORCED).

Before running a single instruction of the handler, the hardware does three things:

1. It pushes eight words onto the stack in use, here the PSP: r0-r3, r12, LR, the PC of the
   faulting instruction, and xPSR. This is the **exception frame**.
2. It sets the fault status registers. `CFSR` (Configurable Fault Status Register) gets bit 16,
   the undefined-instruction bit.
3. It loads a special value, **EXC_RETURN**, into LR. `0xFFFFFFFD` means "return to thread mode,
   on the PSP, with a basic frame".

Then it fetches the handler from the vector table. The vector table is the same on every
ARMv7-M chip in the tree: `KICKOS_CORE_VECTORS_V7M` routes HardFault, MemManage, BusFault and
UsageFault (vectors 3 to 6) all to one function. On this core (no stack limit registers) it is:

```
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        KICKOS_ARMV7M_LSPACT_CLEAR
        "tst lr, #4          \n"
        "ite eq              \n"
        "mrseq r0, msp       \n"
        "mrsne r0, psp       \n"
        "mov r1, lr          \n"
        "bl kickos_armv7m_fault_report \n"
        "bx r0               \n");
}
```

It is `naked`, so no compiler prologue moves SP before it is read. It first drops any lazy FP
save the thread left pending (`KICKOS_ARMV7M_LSPACT_CLEAR`, empty on a core without FP). Bit 2
of EXC_RETURN says which stack holds the frame. The handler passes the frame's address and
EXC_RETURN to the C reporter. The reporter returns only when it contained the fault, with the
EXC_RETURN to leave by, and `bx r0` is then the exception return.

**Try it:**

```
(gdb) break HardFault_Handler
(gdb) continue
(gdb) info registers lr psp control xpsr
lr             0xfffffffd          -3
psp            0x20027f98          537034648
control        0x1                 1
xpsr           0x61000006          1627389958
(gdb) p/x *(unsigned*)0xE000ED28
$1 = 0x10000
(gdb) p/x *(unsigned*)0xE000ED2C
$2 = 0x0
(gdb) p/x *(unsigned*)0xE000ED24
$3 = 0x70008
(gdb) x/8wx $psp
0x20027f98:	0x0000001b	0x00000000	0x00000000	0x00000003
0x20027fa8:	0x00000001	0x000140bd	0x00013e80	0x61000000
(gdb) info symbol 0x13e80
main + 8 in section .kickos_image2_text
(gdb) x/i 0x13e80
   0x13e80 <main(int, char**)+8>:	udf	#0
```

`CONTROL = 1`: nPRIV is set, the thread was unprivileged (exception entry does not change
CONTROL). The low bits of xPSR say exception 6, UsageFault. `0xE000ED28` is CFSR (`0x10000`,
the undefined instruction), `0xE000ED2C` is HFSR (`0x0`: nothing escalated), `0xE000ED24` is
SHCSR (MemManage, BusFault and UsageFault enabled, `0x70000`, plus `0x8`, UsageFault active).
The seventh word of the frame is the faulting PC, `0x13e80`, the `udf` in the faulter program's
`main`, in its own image's code. The eighth is the xPSR the thread had, whose exception number
is 0: it was in thread mode.

**Why, and the contract:** [Book: Interrupts and traps](../book/interrupts-and-traps.md),
[Reference: porting, "Fault-reporter contract"](../reference/porting.md#fault-reporter-contract-panic-must-survive-console-handover)
("Decode BOTH fault banks")

### Stop 3: is it the thread's fault, or the kernel's?
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `kickos_armv7m_fault_report`, `arch_fault_is_user_thread`,
`armv7m_user_frame_trusted`; `arch/arm/common/exc_return.h`, `kickos_arm_exc_return_thread_psp`;
`kernel/init/fault.cc`, `kickos_fault_kill_thread`, `kickos_fault_frame_trusted`, `kickos_fault_stack_top`

The first thing the reporter does, before any output, is ask whether this fault can be
contained:

```
    else if (kickos_arm_exc_return_thread_psp(exc_return) and kickos_fault_kill_thread(frame))
    {
        return KICKOS_ARM_FAULT_EXIT_EXC_RETURN;
    }
    kickos_fault_report_begin();
    kpanic_enter();
```

(The branch above it handles a fault taken from Non-secure state, on ARMv8-M.)
`kickos_fault_kill_thread` (`kernel/init/fault.cc`) is the kernel's half of the decision, and
it starts with the arch's half:

```
extern "C" bool kickos_fault_kill_thread(void* frame)
{
    if (not arch_fault_is_user_thread(frame))
    {
        return false;
    }
    ::kickos::Thread* const c = ::kickos::sched::current();
    // ...
    if (c == nullptr or c == ::kickos::sched::idle())
    {
        return false;
    }
    // ...
    if (c->dying)
    {
        return false;
    }
    arch_fault_redirect_to_exit(frame);
    return true;
}
```

The kill is decided on register state and on the kernel's own records, never on the frame:

1. **The fault came from thread mode on the PSP.** EXC_RETURN says so
   (`kickos_arm_exc_return_thread_psp`). A fault inside another handler is the kernel's.
2. **`CONTROL.nPRIV` is set.** The CPU was unprivileged when it faulted. This is a test on
   the CPU, not on the thread: a system call runs privileged on behalf of the thread
   ([The first syscall](02-first-syscall.md)), and a fault there is a kernel bug in code the
   thread merely called. It must panic.
3. **The thread has somewhere to die.** `kickos_fault_stack_top` answers the top of the
   thread's own kernel block (`ctx.kernel_sp`), where the exit will run.
4. **There is a current thread, it is not idle, and it is not already dying.**
   `Thread::dying` is set at the very start of the exit path, so a second fault during the exit
   goes to the panic dump instead of looping.

The frame plays no part. A thread can point SP anywhere before it faults, or overflow its
stack so that the core's own push of the frame faults. Either way the thread is unprivileged in
thread mode, so it dies and the system runs on. The frame decides only what the record can
say. `armv7m_user_frame_trusted` believes it only when the core stacked it whole (no `MSTKERR`
or `STKERR` bit in CFSR, `armv7m_fault_frame_readable`) and it lies inside the thread's own
stack (`kickos_fault_frame_trusted`). A frame written in full at a wild SP sets no CFSR bit,
which is why both tests are needed. When it is not believed, the record carries no PC.

On ARMv6-M one case is out of reach: a fault while the core is entering HardFault locks the
core up, and no handler runs at all. That is a hardware limit ([12. ARMv6-M](12-armv6m.md)).

**Try it:** continue from Stop 2, or start again:

```
(gdb) break kickos_fault_kill_thread
(gdb) continue
(gdb) p $k->currents.cells[0]->name
$1 = "faulter\000\000\000\000\000\000\000\000"
(gdb) p $k->currents.cells[0]->dying
$2 = false
(gdb) finish
0x000005cc in kickos_armv7m_fault_report (frame=0x20027f98, exc_return=4294967293) at .../arch/arm/armv7m/arch_armv7m.cc:522
Value returned is $3 = true
```

The reporter runs in handler mode, so gdb can read kernel memory here even though the thread
that faulted was unprivileged.

Now run `faultsurvive_ovf`, the same source with `KICKOS_FS_MODE=1`: `faulter` recurses until
it runs off the bottom of its stack. The MPU refuses the write below the stack, so the core
faults while pushing the exception frame itself (`MSTKERR`). The thread is still killed:

```
[fs] worker about to fault

=== THREAD FAULT === thread 'faulter' killed, system continues
  PC unknown CFSR=0x92
  ADDR=0x20025fc0
[fs] survivor ran after the fault
```

`0x92` is `MSTKERR` (bit 4) and `MMARVALID` (bit 7). There is no frame to believe, so the PC is
unknown, and `ADDR` is the address below `faulter`'s stack where the push was refused.

A wild PSP can also meet the kernel before any fault: the context switch and the system call
entry check the PSP of the thread they save before they write a frame through it
(`kickos_arm_bad_psp` in `arch/arm/common/arch_arm_common.cc`). A PSP that leaves no room
for the frame is refused, and the kernel slays that thread alone. Nothing prints at the
refusal. The slain thread's own exit (`kickos_thread_slay_exit` in `kernel/sched/sched.cc`)
prints the record, which names the thread, its PSP, which test refused it, where, the bytes it
needed and the stack's bounds. `pspguard_below` (`user/apps/common/pspguard/main.cc`) points a
thread's PSP under its stack and spins until a switch:

```
=== ARMV7M CONTAINED (wild PSP) ===
  thread 'wild' PSP=0x20018fe0
  under stack_lo, in PendSV need=32 stack=[0x20019180,0x20019800)
[pspguard] contained: the wild thread was slain and main outlived it
```

**Why, and the contract:** [Reference: invariants, `fault-kills-the-unprivileged-thread-task` and `fault-frame-untrusted-until-proved`](../reference/invariants.md),
[Book: Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md)

### Stop 4: record the facts, print nothing, bend the return
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `arch_fault_redirect_to_exit`; `kernel/init/fault.cc`,
`kickos_fault_record`, `kickos_fault_record_no_pc`, `kickos_fault_stack_top`;
`arch/arm/common/fault_resume.h`, `kickos_arm_fault_frame_at`, `kickos_arm_fault_resume_privileged`

The fault handler must not print. Printing from a fault handler means calling `kpanic_enter`,
which masks interrupts and takes the console back from any user driver, for good. That is
right for a dying system and wrong for one that is meant to keep running. So
`arch_fault_redirect_to_exit` only **records** the facts, and **builds a new frame** so that
the exception return lands somewhere new:

```
    else if (armv7m_user_frame_trusted(frame, cfsr))
    {
        kickos_fault_record("CFSR", cfsr, f[6], addr, addr_valid);
    }
    else
    {
        kickos_fault_record_no_pc("CFSR", cfsr, addr, addr_valid);
    }
    // ...
    kickos::arm::reg32(SCB_SHCSR) = shcsr & ~(SHCSR_USGFAULTPENDED | SHCSR_MEMFAULTPENDED
                                              | SHCSR_BUSFAULTPENDED | SHCSR_SVCALLPENDED);
    // Write-1-to-clear and sticky: a bit left set mislabels the NEXT thread's fault.
    kickos::arm::reg32(SCB_CFSR) = cfsr;
    kickos::arm::reg32(SCB_HFSR) = hfsr;
    // ...
    uint32_t const top = kickos_arm_fault_exit_top();
    // ...
    kickos_arm_fault_frame_at(top);
    kickos_arm_fault_resume_privileged();
```

Step by step:

- **The record.** `kickos_fault_record` stores the status register's name and value, the
  faulting PC when the frame is believed, and the faulting address when the core latched a
  valid one (MMFAR for an MPU fault, BFAR for a bus fault; neither for this one). There is one
  record per core, and it remembers which thread it belongs to (`FaultRecord::owner`), because
  the window before it is printed can be preempted and another thread's fault could overwrite
  it.
- **Drop what is pending.** A stacking fault leaves another fault pending, and a fault taken
  over an `svc` leaves the SVCall pending. Both belong to the dying thread, and either would
  otherwise run on the exit's frame, privileged. A dropped SVCall is noted in the record.
- **Clear the status bits.** CFSR and HFSR are sticky and cleared by writing 1s. Left set, they
  would label the next thread's fault with this one's cause.
- **Clear the FP state** on a core with FP: the pending lazy save, and the registers, so the
  dead thread's values reach nobody.
- **Build the exit's frame.** `kickos_arm_fault_frame_at` builds a basic frame at the top of
  the thread's kernel block (`kickos_fault_stack_top`, which answers `ctx.kernel_sp`): zeros,
  PC `kickos_thread_fault_exit`, and an xPSR with only the Thumb bit. It points the PSP at it,
  so the return pops nothing from the thread's own stack, which a task-mate could rewrite.
- **Become privileged on return.** Exception return restores the frame but not `CONTROL`.
  Clearing `CONTROL.nPRIV` now means the thread comes back from the exception privileged.

**Try it:**

```
(gdb) break kickos_fault_record
(gdb) continue
Breakpoint 1, kickos_fault_record (status_name=0xdad4 "CFSR", status=65536, pc=81536, addr=0, addr_valid=0) at .../kernel/init/fault.cc:82
(gdb) p/x $k->currents.cells[0]->ctx.kernel_sp
$1 = 0x20001710
```

`pc=81536` is `0x13e80`, the `udf`: the frame was believed. In a fresh session, stop at the
kill decision instead and let the redirect run:

```
(gdb) break kickos_fault_kill_thread
(gdb) continue
(gdb) finish
Value returned is $1 = true
(gdb) info registers psp control
psp            0x200016f0          536876784
control        0x4                 4
(gdb) x/8wx $psp
0x200016f0 <...system_pools+2992>:	0x00000000	0x00000000	0x00000000	0x00000000
0x20001700 <...system_pools+3008>:	0x00000000	0x00000000	0x000050a4	0x01000000
(gdb) info symbol 0x50a4
kickos_thread_fault_exit in section .text
```

The PSP now names the built frame, 32 bytes below the kernel block's top (`0x20001710`), whose
PC is the exit. `CONTROL` has nPRIV clear; bit 2 (FPCA) is set because the redirect itself used
the FP registers to clear them.

**Why, and the contract:** [Reference: invariants, `fault-kill-path-never-enters-panic` and `fault-record-is-printed-only-by-its-owner`](../reference/invariants.md),
[Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond)
("The two seams", "The handler MUST NOT PRINT")

### Stop 5: the dying thread walks into its own exit
**Where:** `kernel/init/fault.cc`, `kickos_thread_fault_exit`, `fault_record_print`

`kickos_fault_kill_thread` returns true, the reporter returns `KICKOS_ARM_FAULT_EXIT_EXC_RETURN`
(`0xFFFFFFFD`), and the handler's `bx r0` is the exception return. The core pops the built frame
and resumes in thread mode, privileged, on the PSP, in `kickos_thread_fault_exit`, with SP at
the top of the thread's kernel block. Nothing runs on the user stack from here on. That
matters: another thread of the same task shares the domain and could overwrite frames on the
user stack while the exit path walks the kernel's data structures.

`kickos_thread_fault_exit` runs as the dying thread, in thread context, with interrupts
enabled. This is an ordinary kernel path now, not an exception handler, and it may print.
`fault_record_print` prints the record through `kprintf_record` (a path that still reaches a
user console driver if one owns stdout, see
[Reference: console, "A fault record while a driver owns the console"](../reference/console.md#a-fault-record-while-a-driver-owns-the-console)),
then the thread exits:

```
    fault_record_print(c);
    ::kickos::krecord_end();
    ::kickos::sched::exit_current(KOS_EXIT_FAULT, ::kickos::sched::EXIT_FAULTED);
```

The record is the `THREAD FAULT` banner with the thread's name, then one line with the PC and
the status register (`PC unknown` where the frame was not believed), then the address line
where the core latched one. A record another thread's later fault has taken prints
`PC lost to a later fault` instead.

`KOS_EXIT_FAULT` is 139. It is the exit status of the task, which the task's creator can read
later (`kos_task_state`), and of the whole program if this was the last thread.

**Try it:**

```
(gdb) break kickos_thread_fault_exit
(gdb) continue
(gdb) info registers sp control xpsr
sp             0x20001710          0x20001710 <kickos::(anonymous namespace)::system_pools+3024>
control        0x2                 2
xpsr           0x1000000           16777216
(gdb) p/x $k->currents.cells[0]->ctx.kernel_sp
$1 = 0x20001710
```

`CONTROL = 2` means SPSEL is set (thread mode uses the PSP) and nPRIV is clear (privileged).
The exception number in xPSR is 0, thread mode. SP is the kernel block's top: the built frame
has been popped.

**Why, and the contract:** [Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond),
[Reference: invariants, `fault-exit-status-reaches-the-creator`](../reference/invariants.md)

### Stop 6: exit, part one: mark it dying, end the task
**Where:** `kernel/sched/sched.cc`, `sched::exit_current`; `kernel/task/task.cc`, `task_end`,
`mark_ended`, `task_release`, `task_orphan_created_by`; `kernel/thread/park.cc`,
`task_cancel_mark`, `task_cancel_parks`

`exit_current` is the one exit path. A thread that returns from its function, one that calls
`kos_exit`, one that is slain, and one that faulted all end here. The fault reaches it with
`cause = EXIT_FAULTED`. The first block runs under `IrqLock`:

- `c->dying = true`. From here a second fault panics (Stop 3).
- `notify_unbind_self`: if the thread was bound to a notification, unbind it, so no wake can
  land on a thread already in teardown.
- **End the task.**

```
                if (cause == EXIT_FAULTED or cause == EXIT_TASK or c->task_role == TASK_ROLE_ENTRY)
                {
                    task_end(c->task, code, c->cancel_kind == CANCEL_NONE, lock);
                }
                else if (c->task_role == TASK_ROLE_ESSENTIAL)
                {
                    task_end(c->task, KOS_EXIT_FAULT, true, lock);
                }
```

  `task_end` calls `mark_ended`, which sets the task's `ENDED` mark, stores the exit status
  (139), and raises the task's watch if anyone set one (Stop 9). If the task has other
  members, `task_cancel_mark` marks every one of them slain and `task_cancel_parks` ends their
  parks. A faulting thread takes the rest of its task with it. `faulter` is alone in its task,
  so nobody else is cancelled here.

- `mutex_owner_exit` hands over the mutexes it holds (Stop 7).
- `c->mpu.drop_devices()`: give back any device window the thread held, so a supervisor that
  restarts a driver can claim the device again.
- `task_release`: the thread leaves its task. It is counted as **sweeping** until its
  capabilities are closed (next stop), so the task slot is not reused under it.
- `task_orphan_created_by`: any task this thread created loses its creator. A dead thread's
  pool slot will be reused, and the new occupant must not inherit power over tasks it never
  made.

**Try it:**

```
(gdb) break kickos::sched::exit_current
(gdb) continue
(gdb) set $c = $k->currents.cells[0]
(gdb) p $c->name
$1 = "faulter\000\000\000\000\000\000\000\000"
(gdb) p code
$2 = 139
(gdb) p cause
$3 = kickos::sched::EXIT_FAULTED
(gdb) p $c->task_role
$4 = 1 '\001'
(gdb) break kickos::task_end
(gdb) continue
(gdb) set $t = t
(gdb) p $t->refcount
$5 = 1 '\001'
(gdb) p $t->marks
$6 = 128 '\200'
(gdb) finish
(gdb) p $t->marks
$7 = 136 '\210'
(gdb) p $t->exit_status
$8 = 139
(gdb) p $c->dying
$9 = true
```

`task_role` 1 is `TASK_ROLE_ENTRY`: `faulter` was the first thread seated in its task. The
marks go from 128 (started) to 136: started and `ENDED` (8, `KOS_TASK_ENDED` in
`user/include/kickos/sys/abi.h`).

**Why, and the contract:** [Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond)
("The rule"), [Reference: invariants, `fault-exit-status-reaches-the-creator`](../reference/invariants.md)

### Stop 7: exit, part two: release every capability
**Where:** `kernel/syscall/cap.cc`, `cap_teardown`, `teardown_entry`, `obj_close_protocol`, `obj_ref_drop`;
`kernel/sched/sched.cc`, `exit_current`; `kernel/sync/sync.cc`, `mutex_owner_exit`

With the lock released, `exit_current` calls `cap_teardown`. It closes every capability the
thread holds, through the same per-type protocol and reference drop as `handle_close`
([Spawn and capabilities, Stop 13](08-spawn-and-caps.md)), with one difference: `teardown = true`.

```
        void teardown_entry(Thread* c, uint32_t i, Held held)
        {
            CapEntry& e = *cap_slot(c->caps, i);
            obj_close_protocol(c, e, /*teardown=*/true, held);
            CapEntry const detached = e;
            cap_slot_vacate(c, i, &e);
            obj_ref_drop(detached, /*teardown=*/true, held);
        }
```

What the close protocol does, type by type:

- **An endpoint.** As on a close: if this was the last capability that could receive, every
  sender parked on it is answered `-KOS_ECONNREFUSED` at once.
- **A reply capability.** If the thread was a server holding a caller's reply capability, the
  caller is woken with `-KOS_EPIPE` ([Call and reply](05-call-and-reply.md)).
- **An IRQ capability.** These are released first, all in one locked pass, so a supervisor
  woken by this death can claim the line again at once.

The rest is swept `KCAP_TEARDOWN_CHUNK` (4) slots at a time, with the lock dropped between
chunks: a big table must not keep interrupts masked for long. And `obj_ref_drop` with
`teardown = true` follows one more rule: if this was the last reference but a waiter is still
parked on the object, the object is **leaked** (kept with one reference) rather than freed
under the waiter.

A mutex is not in this list, because it is no capability: it is a word in user memory. The
kernel knows of a held mutex only while a thread waits for it, as a record on the holder's held
list, and `exit_current` empties that list before the sweep, in the locked section that marks
the thread dying (`mutex_owner_exit`). Each record goes to its highest waiter: the kernel
writes that waiter's id into the word, through the waiter's own view of it, and the waiter's
`kos_mutex_lock` returns `-KOS_EOWNERDEAD`, so it knows the protected data may be inconsistent.
Nobody is left waiting on a dead owner. A mutex the dying thread held with nobody waiting keeps
its id in the word, and the next thread to take it finds that id dead and is told the same way.
[4. A mutex](04-mutex.md) walks the waiter side.

**Try it:** `faulter` holds no capability at all, and no mutex record.

```
(gdb) break kickos::cap_teardown
(gdb) continue
(gdb) p c->cap_width
$1 = 16
(gdb) p *c->caps[0]@2
$2 = {{obj = 0, obj_gen = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0, badge = 0}, {obj = 1048579, obj_gen = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0, badge = 0}}
(gdb) p c->held_list
$3 = (kickos::WordRecord *) 0x0 <g_isr_vector>
(gdb) p $k->word_active
$4 = (kickos::WordRecord *) 0x0 <g_isr_vector>
```

Both slots are empty (the second holds a free-list link). The thread holds no mutex record, and
no thread waits on any mutex word (the kernel's list of live records, `word_active`, is empty).
There is nothing for this sweep to release.

The `selftest` image has a thread `odOwn` exit while it holds a mutex that `odWt` wants (its
test `t_mutex_owner_died`, `user/apps/common/selftest/main.cc`). That is an ordinary exit, not
a fault, but the teardown is the same code. `odWt` is told `-KOS_EOWNERDEAD` either way: by the
hand-over above when it is parked on the word as `odOwn` exits, or, when it reaches the word
only after the exit, by its own lock finding the dead id there. On this build the run takes the
second road: stop at `break kickos::sched::exit_current if $_streq($k->currents.cells[0]->name,
"odOwn")`, and `p $k->currents.cells[0]->held_list` prints `0x0`.

**Why, and the contract:** [Book: Adding a kernel object type](../book/adding-a-kernel-object-type-the-additive-recipe.md)
(the close protocol and the teardown flag), [Book: Priority inheritance](../book/priority-inheritance-lending-urgency.md)
(owner-died), [Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
("Lifecycle")

### Stop 8: exit, part three: wake who waits, report the death, switch away
**Where:** `kernel/sched/sched.cc`, `sched::exit_current`; `kernel/thread/park.cc`, `thread_abort_park`;
`kernel/task/task.cc`, `task_sweep_done`, `task_report_death`; `kernel/include/kickos/thread.h`,
`ThreadPool::retire`, `ThreadPool::alloc`

The last block of `exit_current`, under the lock again:

- The capability run goes back with its task's budget, and the thread is retired
  (`ThreadPool::retire`). Its memory comes back later: the slot is reclaimed by the next
  `ThreadPool::alloc`, which only then pushes a kernel-owned stack back on the free list
  ([Spawn and capabilities, Stop 7](08-spawn-and-caps.md)), because only then is the thread
  provably off the CPU.
- `task_sweep_done`: this member's sweep is finished. If it was the last member and the last
  sweep, the task is **dead**.
- Every thread parked in `kos_thread_join` on this thread is woken (`thread_abort_park`) with
  result 0. A joiner learns that the thread is gone, not how it died; the status is the
  creator's to read from the task.
- `task_report_death` sets the task's `DEAD` and `ENDED` marks and raises its watch. A task its
  creator already slew loses the creator's hold here (Stop 10).
- If no thread is left at all, `kickos_terminate` ends the system with this exit code.
  Otherwise `reschedule` switches to someone else, and this thread never runs again.

`main` is not parked on anything the death wakes: it sleeps between its reads of the task's
state, and its next read finds `DEAD`.

**Try it:**

```
(gdb) break kickos::task_report_death
(gdb) continue
(gdb) set $t = t
(gdb) p $t->marks
$1 = 136 '\210'
(gdb) p $t->refcount
$2 = 0 '\000'
(gdb) finish
(gdb) p $t->marks
$3 = 140 '\214'
```

The task has no member left, and its marks go from started and `ENDED` (136) to started,
`DEAD` and `ENDED` (140).

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
(the park and the wake), [Reference: invariants, `task-scoped-death-and-a-cancel-that-reaches-every-park`](../reference/invariants.md)

### Stop 9: whoever watches is told
**Where:** `kernel/notify/watch.cc`, `watch_call`, `watch_task`; `kernel/task/task.cc`,
`mark_ended`, `watch_raise`, `task_state_call`; `system/init/compose/walk.cc`, `Walk::wake`,
`Walk::death`, `Walk::end`

`faulter`'s task has no watch: `main` reads its state. The init, though, watches every task
it starts ([Spawn and capabilities, Stop 4](08-spawn-and-caps.md)): `kos_watch` stores a
notification and a bit in the task, and `watch_raise` sets that bit whenever the task ends,
dies, or becomes ready, as the watch asked. The init sleeps in `kos_notify_wait`, and the bit
wakes it.

`faulter`'s death does not touch the init. To see the watch fire, use `fault`
(`user/apps/common/fault/main.cc`), where it is `main` itself that executes the undefined
instruction. Everything up to here happens the same way, to `main`. Then the init wakes and
asks the kernel what happened:

```
            int status = 0;
            int const state = kos_task_state(r.handle, &status);
            // ...
            bool const ended = (state & KOS_TASK_ENDED) != 0;
            if (i == ends_ and ended)
            {
                ending = true;
                ends_status = status;
                continue;
            }
            if ((state & KOS_TASK_DEAD) != 0)
            {
                check(kos_task_slay(r.handle), "kos_task_slay", i);
                report(Report::Kind::RELEASED, i, nullptr, 0);
                death(i);
                continue;
            }
```

The bit says only where to look; the task's state says what happened. `main` is the task the
composition's `ends:` names, so its end is the end of the system: the init reads its exit
status (`kos_task_state` writes it, 139) and calls `Walk::end`, which shuts down with that
status. A task that is not `ends:` and is found `DEAD` is instead released with
`kos_task_slay`, and `Walk::death` decides from its restart budget whether to start it again.
A supervisor written by hand does the same with its own notification.

**Try it:** on `fault`:

```
(gdb) break kickos::sched::start
(gdb) continue
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) delete
(gdb) break kickos::task_end
(gdb) continue
(gdb) p t->watch_notify
$1 = 1
(gdb) p/x t->watch_bit
$2 = 0xc0
(gdb) break kickos::notify_raise
(gdb) continue
(gdb) bt 4
#0  kickos::notify_raise (n=0x20002dec <...>, bit=1) at .../kernel/notify/notify.cc:187
#1  0x000032c6 in kickos::task_end (t=0x20002d44 <...>, code=139, latch=true, held=...) at .../kernel/task/task.cc:570
#2  0x0000c83c in kickos::sched::exit_current (code=code@entry=139, cause=cause@entry=kickos::sched::EXIT_FAULTED, held=held@entry=0x0 <g_isr_vector>) at .../kernel/sched/sched.cc:945
#3  0x000050d6 in kickos_thread_fault_exit () at .../kernel/init/fault.cc:403
(gdb) delete
(gdb) break kickos::task_state_call
(gdb) continue
(gdb) p $k->currents.cells[0]->name
$3 = "root", '\000' <repeats 11 times>
(gdb) set $s = status
(gdb) finish
Value returned is $4 = 12
(gdb) p *$s
$5 = 139
(gdb) continue
[Inferior 1 (process 1) exited with code 0213]
```

`mark_ended` and `watch_raise` are inlined into `task_end`. `watch_bit` holds the bit, 0, in its
low bits, and `bit=1` is the mask `1u << 0`. The init (running as root) reads state 12,
`DEAD | ENDED`, and status 139. gdb prints the exit code in octal: `0213` is 139.

**Why, and the contract:** [Reference: invariants, "App entry & init seam"](../reference/invariants.md#app-entry--init-seam),
[Reference: invariants, `fault-exit-status-reaches-the-creator`](../reference/invariants.md)

### Stop 10: the survivor runs on
**Where:** `user/apps/common/faultsurvive/main.cc`, `main`; `kernel/syscall/syscall_thread.cc`, `task_slay`;
`kernel/task/task.cc`, `task_stop_for_slay`, `task_drop_hold`

Back on `faultsurvive`. `main`'s `wait_dead` finds the task `DEAD` and returns 0. `main` then
calls `kos_task_slay(victim)`. The task is already dead, so there is nobody to stop. What
remains is the **creator's hold**: a task that its creator still names keeps its slot even
with no member, so the creator can read its state. `task_stop_for_slay` finds no member and no
sweep left and calls `task_drop_hold`, which gives that up. With no member left, the slot goes
back: its domain is released and its generation bumped, so the old handle `3` stops resolving.
Then `main` closes its program capability, prints its line and returns 0, which ends the system.

The task is not a capability, so this is not a `handle_close`. It is the creator's authority,
checked by `task_created_by`.

**Try it:**

```
(gdb) break kickos::sched::start
(gdb) continue
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) set $P = &kickos::kickos_kernel_pools
(gdb) delete
(gdb) break kickos::task_slay
(gdb) continue
(gdb) p $k->currents.cells[0]->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p/x task
$2 = 0x3
(gdb) p/x $P->tasks[2]
$3 = {domain = 0x20003294, free_node = {next = 0x0, prev = 0x0}, members = {head = 0x0}, refcount = 0x0, creator_tag = 0x2, gen = 0x0, prio_ceiling = 0xa, marks = 0x8c, watch_bit = 0x0, sweeping = 0x0, image = 0x2, watch_notify = 0x0, watch_ep = 0x0, exit_status = 0x8b, reent_seat = 0x20018000}
(gdb) finish
Value returned is $4 = 0
(gdb) p/x $P->tasks[2]
$5 = {domain = 0x0, free_node = {next = 0x0, prev = 0x0}, members = {head = 0x0}, refcount = 0x0, creator_tag = 0x0, gen = 0x1, prio_ceiling = 0xa, marks = 0x8c, watch_bit = 0x0, sweeping = 0x0, image = 0x2, watch_notify = 0x0, watch_ep = 0x0, exit_status = 0x8b, reent_seat = 0x20018000}
(gdb) p $P->threads[2].state
$6 = {v_ = kickos::ThreadState::EXITED}
(gdb) continue
[Inferior 1 (process 1) exited normally]
```

Before the slay the task is dead (`0x8c`: started, `DEAD`, `ENDED`) with exit status 139
(`0x8b`). After it, the slot has no domain and no creator, and its generation is 1.
`faulter`'s pool slot (2) is EXITED, waiting for the next spawn to reclaim it. The program
exits with status 0.

**Why, and the contract:** [Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond),
[Reference: architecture, "Memory domains (the isolation unit)"](../reference/architecture.md#memory-domains-the-isolation-unit)

## The other road: a fault the kernel does not contain, and the kernel's own panic

Everything above depends on Stop 3 saying yes. When it says no, the same reporter takes the
other branch, and the system stops.

### Stop 11: a fault that panics
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `kickos_armv7m_fault_report`; `kernel/init/console.cc`,
`kpanic_enter`; `arch/common/kfault_terminate_shutdown.cc` and `arch/common/kfault_terminate_default.cc`,
`kfault_terminate`

A fault in privileged code is the kernel's own: a system call that faults, an interrupt
handler, the kernel's exit path. So is a second fault in a thread that is already dying. None
of these happens in a working image, and no shipped QEMU image makes one, so this stop forces one
from gdb: it marks `faulter` dying by hand before the kernel decides, and Stop 3's last test then
refuses. The reporter goes on:

- `kpanic_enter` masks interrupts for good, takes the console back from any user driver, and
  flushes whatever is queued.
- It prints the dump: a label from CFSR (`MPU FAULT`, `BUS FAULT`, `USAGE FAULT` or
  `HARD FAULT`), the frame if it could be read, CFSR and HFSR, and MMFAR or BFAR when valid.
- `kfault_terminate` ends everything. On QEMU the chip opts into
  `arch/common/kfault_terminate_shutdown.cc`, which exits the emulator with status 132. On the
  f411 the fallback `arch/common/kfault_terminate_default.cc` blinks the diagnostic LED forever.

**Try it:** on `faultsurvive`:

```
(gdb) break HardFault_Handler
(gdb) continue
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) set var $k->currents.cells[0]->dying = true
(gdb) break kickos_fault_kill_thread
(gdb) break kpanic_enter
(gdb) break kfault_terminate
(gdb) continue
(gdb) finish
Value returned is $1 = false
(gdb) continue
Breakpoint 3, kpanic_enter () at .../kernel/init/console.cc:1163
(gdb) continue
Breakpoint 4, kfault_terminate () at .../arch/common/kfault_terminate_shutdown.cc:19
```

The console shows the dump, and QEMU exits with 132:

```
[fs] worker about to fault

=== USAGE FAULT ===
  PC=0x13e80 LR=0x140bd xPSR=0x61000000 (PSP)
  R0=0x1b R1=0x0 R2=0x0 R3=0x3 R12=0x1
  CFSR=0x10000 HFSR=0x0
```

**Why, and the contract:** [Reference: porting, "Fault-reporter contract"](../reference/porting.md#fault-reporter-contract-panic-must-survive-console-handover),
[Reference: invariants, `fault-frame-untrusted-until-proved`](../reference/invariants.md)

### Stop 12: the kernel's own panic
**Where:** `kernel/include/kickos/kernel.h`, `KICKOS_ASSERT`; `kernel/init/console.cc`, `kpanic`,
`kickos_panic_report`; `arch/arm/armv7m/switch.S`, `kickos_panic_stack_enter`

A fault is the hardware's verdict. A **panic** is the kernel's own: an invariant that cannot
hold any more. `KICKOS_ASSERT(cond)` calls `kpanic` when `cond` is false, and root or a user
thread holding `AUTH_SYSTEM` can ask for the same end with `kos_panic` (the `KOS_SYS_PANIC`
arm, `user_panic_system`, which copies the message in with the same per-byte checks as any user
string). Without the bit the same call ends only the caller's task, with `KOS_EXIT_PANIC`.
`kpanic` does not trust the stack it was called on: `kickos_panic_stack_enter` (assembly) moves
SP to a dedicated panic stack (`g_panic_stack`, one per core) and calls `kickos_panic_report`,
which calls `kpanic_enter` and prints `KERNEL PANIC:` with the message. Then `kfault_terminate`,
as above.

There is no kill and no survivor on this road. The panic path is for the case where the kernel
can no longer vouch for its own state; containing a fault is for the case where it still can.

**Try it:** on `panicgate1` (`user/apps/common/panicgate/main.cc`, case 1), whose `main` calls
`kos_panic`:

```
(gdb) break kickos_panic_report
(gdb) break kfault_terminate
(gdb) continue
Breakpoint 1, kickos_panic_report (msg=0x20001290 <...> "[panicgate] message on the wire", file=0x0 <g_isr_vector>, line=0) at .../kernel/init/console.cc:1117
(gdb) bt 2
#0  kickos_panic_report (msg=... "[panicgate] message on the wire", file=0x0 <g_isr_vector>, line=0) at .../kernel/init/console.cc:1117
#1  0x00004080 in kickos::kpanic (msg=<optimized out>) at .../kernel/init/console.cc:1066
(gdb) info registers sp
sp             0x200001e0          0x200001e0 <(anonymous namespace)::g_line_all>
(gdb) continue
Breakpoint 2, kfault_terminate () at .../arch/common/kfault_terminate_shutdown.cc:19
```

`0x200001e0` is the top of `g_panic_stack` (it starts at `0x200000a0` and is 320 bytes; gdb
names the next symbol). The console shows:

```
KERNEL PANIC: [panicgate] message on the wire
```

and QEMU exits with 132.

**Why, and the contract:** [Reference: porting, "Fault-reporter contract"](../reference/porting.md#fault-reporter-contract-panic-must-survive-console-handover),
[Reference: invariants, `panic-console-probe-independent`](../reference/invariants.md)

## What you have seen

An unprivileged thread's fault arrives as a hardware exception with a frame on the thread's
own stack. Before anything is printed, the kernel decides from the CPU's privilege and mode
and the thread's own state whether the fault is the thread's; the frame decides only whether
the record can name the PC. If the fault is the thread's, the handler records the facts and
builds a frame on the thread's kernel block so that the exception return resumes the dying
thread, privileged, in the one exit path. That path ends the task, hands every mutex it holds
that a thread waits on to that waiter, closes every capability (answering parked callers and
senders), wakes joiners, marks the task dead, and raises the watch. The creator learns the
exit status from the task; the init, if it watches, restarts or ends. The system keeps running.
If any test fails, the same handler panics instead.

## Where to next

- [10. The MMU](10-the-mmu.md): the next chapter, the same walk on a part with page tables instead
  of an MPU.
- [8. Spawn and capabilities](08-spawn-and-caps.md): the forward direction of everything this chapter
  undoes.
- [4. Mutex](04-mutex.md): the waiter that receives `-KOS_EOWNERDEAD`.
- [5. Call and reply](05-call-and-reply.md): the caller that receives `-KOS_EPIPE` when its
  server dies.
- [12. ARMv6-M](12-armv6m.md): the same stops on a core with no CFSR.
- [14. RISC-V](14-riscv.md): the same decision read from `mstatus.MPP`, with the frame built on
  the kernel stack by software.

## Exercises

1. **A fault in the caller's task.** Move `faulter`'s code into a thread `main` spawns with
   `kos::thread::create`, so it joins `main`'s task. Predict who prints after the fault, and the
   QEMU exit status. Check with `break kickos::task_cancel_mark` and `p t->refcount` (it is now
   more than one).
2. **An MPU fault with an address.** Run `mpu_fault` (`user/apps/common/mpu_fault/main.cc`),
   whose thread writes just past its granted region. Predict the CFSR bits and whether
   `kickos_fault_record` gets `addr_valid = 1`. Check at `break kickos_fault_record`, and
   compare the address with the `[domain] expect fault at` line the app prints.
3. **A fault during the exit.** Reason through what Stop 3's `dying` test does if a thread
   faults again inside `cap_teardown`. Which branch of `kickos_armv7m_fault_report` runs, and
   why is that safer than a second redirect?
