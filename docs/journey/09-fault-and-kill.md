<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 9. Fault and kill: a user fault, the report, the kill, who survives

By the end of this chapter you will have followed an unprivileged thread that executes an
undefined instruction, from the moment the Cortex-M core takes the exception to the moment a
different thread prints that it survived. You will see the hardware's own work (the stacked
frame, the fault status registers), the three tests the kernel makes before it decides the
fault is the thread's and not its own, why the fault handler prints nothing, how the exception
return is bent so that the dying thread walks into its own exit, and what that exit releases:
capabilities, mutexes, waiters, the task. You will see how other threads learn about the
death and how the system continues. A short final section follows a fault the kernel cannot
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
`KICKOS_FS_MODE=0`. Its `main` creates a task, spawns a thread called `faulter` into it, and
joins it. `faulter` prints one line and executes `KOS_TRAP_ILLEGAL()`, which on Arm is the
instruction `udf #0`, permanently undefined. `main` must then print that it survived:

```
[fs] spawning the faulter
[fs] worker about to fault

=== THREAD FAULT === thread 'faulter' killed, system continues
  PC=0x108 CFSR=0x10000
[fs] survivor ran after the fault
```

QEMU then exits with status 0, the status `main` returned. The test that runs it is
`qemu_faultsurvive` (`tests/integration/check_faultsurvive.sh`), which checks that the
survivor line comes after the kill line.

Start it halted and attach, as in [Setup](00-setup.md):

```sh
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/faultsurvive/faultsurvive -gdb tcp::1234 -S
gdb-multiarch build/qemu/user/apps/common/faultsurvive/faultsurvive -ex 'target remote :1234'
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
```

`$k` is the kernel's state object, as in [Spawn and capabilities](08-spawn-and-caps.md). Two more
images appear later: `sysdefault_fault` (Stop 9) and `faultsurvive_ovf` and `panicgate1` (the
last section).

### Stop 1: the victim gets a task of its own
**Where:** `user/apps/common/faultsurvive/main.cc`, `main`; `kernel/syscall/syscall_thread.cc`,
`spawn_masked`

The unit a fault kills is the **task**, not the thread. The threads of one task share one
memory domain, so when one of them faults while writing, the others may be looking at
memory it left half-written; the kernel ends them all. A plain spawn puts the new thread in
the spawner's own task. So a program that wants to outlive a thread's fault must put that
thread in a task of its own:

```
    kos_task_t victim = KOS_TASK_NONE;
    if (kos_task_create(nullptr, 0, 0, &victim) != 0)
    {
        kos::print("[fs] ERROR: no task slot for the faulter\n");
        return 1;
    }
    kos::print("[fs] spawning the faulter\n");
    kos::thread::Handle t = kos::thread::create(faulter, nullptr, "faulter", 10,
                                                KOS_POLICY_FIFO, 0,
                                                nullptr, 0, stack, stack_size,
                                                nullptr, 0, nullptr, 0, 0, nullptr, victim);
    int const rc = t.join(KOS_TIMEOUT_NONE);
```

[Spawn and capabilities](08-spawn-and-caps.md) walked `kos_task_create` and the spawn. Here, notice
only that `main` is the task's creator, which is what will let it kill the task afterwards.

**Try it:** the first spawn is the init starting `main`; the second is `main` starting
`faulter`.

```
(gdb) break kickos::spawn_masked
(gdb) continue
(gdb) continue
(gdb) p $k->currents.cells[0]->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p p->task
$2 = 4
(gdb) p p->prio
$3 = 10 '\n'
(gdb) p $k->tasks[3]
$4 = {domain = 0x20006428 <kickos::detail::g_instance+7288>, refcount = 0 '\000', creator_tag = 2 '\002',
  gen = 0, prio_ceiling = 31 '\037', marks = 0 '\000', ...}
```

Task handle 4 is `tasks[3]`: empty so far, created by `main` (kill tag 2).

**Why, and the contract:** [Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond)
("The rule"), [Reference: invariants, `fault-kills-the-unprivileged-thread-task`](../reference/invariants.md)

### Stop 2: the core takes the exception
**Where:** `arch/arm/common/core_vectors.inc`, `KICKOS_CORE_VECTORS_V7M`; `arch/arm/chip/mps2/startup.S`
(on the f411, `arch/arm/chip/stm32f411/startup.S`); `arch/arm/armv7m/arch_armv7m.cc`, `HardFault_Handler`

`faulter` runs unprivileged, in thread mode, on its own stack (the process stack pointer,
PSP). It executes `udf #0`. The core cannot execute it, so it raises a **UsageFault**. On
this build UsageFault is not enabled as its own exception: the PMSAv7 backend
(`arch/arm/common/arch_arm_mpu_pmsav7.cc`) sets only `SHCSR_MEMFAULTENA` and
`SHCSR_BUSFAULTENA` in `SCB_SHCSR`. A disabled fault **escalates** to HardFault, and the core
records that it did (HFSR.FORCED).

Before running a single instruction of the handler, the hardware does three things:

1. It pushes eight words onto the stack in use, here the PSP: r0-r3, r12, LR, the PC of the
   faulting instruction, and xPSR. This is the **exception frame**.
2. It sets the fault status registers. `CFSR` (Configurable Fault Status Register) gets bit 16,
   `UNDEFINSTR`.
3. It loads a special value, **EXC_RETURN**, into LR. `0xFFFFFFFD` means "return to thread mode,
   on the PSP, with a basic frame".

Then it fetches the handler from the vector table. The vector table is the same on every
ARMv7-M chip in the tree: `KICKOS_CORE_VECTORS_V7M` routes HardFault, MemManage, BusFault and
UsageFault (vectors 3 to 6) all to one function:

```
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4          \n"
        "ite eq              \n"
        "mrseq r0, msp       \n"
        "mrsne r0, psp       \n"
        "mov r1, lr          \n"
        "b kickos_armv7m_fault_report \n");
}
```

It is `naked`, so no compiler prologue moves SP before it is read. Bit 2 of EXC_RETURN says
which stack holds the frame. The handler passes the frame's address and EXC_RETURN to the C
reporter. It branches (`b`), it does not call: the reporter's own return will be the exception
return.

**Try it:**

```
(gdb) break HardFault_Handler
(gdb) continue
(gdb) info registers lr psp control xpsr
lr             0xfffffffd          -3
psp            0x20029fd8          537042904
control        0x1                 1
xpsr           0x61000003          1627389955
(gdb) p/x *(unsigned*)0xE000ED28
$1 = 0x10000
(gdb) p/x *(unsigned*)0xE000ED2C
$2 = 0x40000000
(gdb) p/x *(unsigned*)0xE000ED24
$3 = 0x30000
(gdb) x/8wx $psp
0x20029fd8:	0x0000001b	0x00000000	0x00000001	0x00000003
0x20029fe8:	0x0000000a	0x000014bd	0x00000108	0x61000000
(gdb) info symbol 0x108
(anonymous namespace)::faulter(void*) + 8 in section .text
(gdb) x/i 0x108
   0x108 <(anonymous namespace)::faulter(void*)+8>:	udf	#0
```

`CONTROL = 1`: nPRIV is set, the thread was unprivileged (exception entry does not change
CONTROL). The low bits of xPSR say exception 3, HardFault. `0xE000ED28` is CFSR (`0x10000`,
UNDEFINSTR), `0xE000ED2C` is HFSR (`0x40000000`, FORCED), `0xE000ED24` is SHCSR (MemManage and
BusFault enabled, UsageFault not). The seventh word of the frame is the faulting PC, `0x108`,
the `udf` in `faulter`. The eighth is the xPSR the thread had, whose exception number is 0:
it was in thread mode.

**Why, and the contract:** [Book: Interrupts and traps](../book/interrupts-and-traps.md),
[Reference: porting, "Fault-reporter contract"](../reference/porting.md#fault-reporter-contract-panic-must-survive-console-handover)
("Decode BOTH fault banks")

### Stop 3: is it the thread's fault, or the kernel's?
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `kickos_armv7m_fault_report`, `arch_fault_is_user_thread`;
`kernel/init/fault.cc`, `kickos_fault_kill_thread`, `kickos_fault_frame_trusted`

The first thing the reporter does, before any output, is ask whether this fault can be
contained:

```
    bool const frame_read = armv7m_fault_frame_readable(kickos::arm::reg32(SCB_CFSR));
    if (kickos_fault_kill_thread(frame))
    {
        return;
    }
    kpanic_enter(); // mask IRQs + force the sync path + flush queued bytes, in order
```

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

`arch_fault_is_user_thread` answers yes only if all four of these hold:

1. **`CONTROL.nPRIV` is set.** The CPU was unprivileged when it faulted. This is a test on
   the CPU, not on the thread: a system call runs privileged on behalf of the thread
   ([The first syscall](02-first-syscall.md)), and a fault there is a kernel bug in code the
   thread merely called. It must panic.
2. **The frame was stacked completely.** If the core's own push of the frame faulted (a stack
   overflow is the usual cause), CFSR has MSTKERR or STKERR set, and the frame in memory is
   garbage. `armv7m_fault_frame_readable` tests those bits (mask `0x1818`).
3. **The frame lies inside the thread's own stack.** `kickos_fault_frame_trusted` checks that
   the 32 bytes of the frame sit within `[stack_base, stack_base + stack_size)`. A thread can
   point SP anywhere before it faults; a frame written at a wild SP sets no CFSR bit, so test 2
   cannot catch it.
4. **The stacked IPSR is 0.** The fault was taken from thread mode, not from inside another
   handler.

Then the kernel adds its own two: there is a current thread and it is not idle, and the
thread is not already **dying**. `Thread::dying` is set at the very start of the exit path, so
a second fault during the exit (an overflowing stack can fault again on the way out) goes to
the panic dump instead of looping.

**Try it:** continue from Stop 2, or start again:

```
(gdb) break kickos_armv7m_fault_report
(gdb) continue
(gdb) p/x exc_return
$1 = 0xfffffffd
(gdb) p $k->currents.cells[0]->name
$2 = "faulter\000\000\000\000\000\000\000\000"
(gdb) p $k->currents.cells[0]->dying
$3 = false
(gdb) break arch_fault_is_user_thread
(gdb) continue
(gdb) finish
0x00005858 in kickos_fault_kill_thread (frame=0x20029fd8) at .../kernel/init/fault.cc:177
Value returned is $4 = true
```

The reporter runs in handler mode, so gdb can read kernel memory here even though the thread
that faulted was unprivileged.

**Why, and the contract:** [Reference: invariants, `fault-kills-the-unprivileged-thread-task` and `fault-frame-untrusted-until-proved`](../reference/invariants.md),
[Book: Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md)

### Stop 4: record the facts, print nothing, bend the return
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `arch_fault_redirect_to_exit`; `kernel/init/fault.cc`,
`kickos_fault_record`, `kickos_fault_stack_top`; `arch/arm/common/fault_resume.h`,
`kickos_arm_fault_resume_at`, `kickos_arm_fault_resume_privileged`

The fault handler must not print. Printing from a fault handler means calling `kpanic_enter`,
which masks interrupts and takes the console back from any user driver, for good. That is
right for a dying system and wrong for one that is meant to keep running. So
`arch_fault_redirect_to_exit` only **records** the facts, and **rewrites the frame** so that
the exception return lands somewhere new:

```
    uint32_t* const f = static_cast<uint32_t*>(frame);
    kickos_fault_record("CFSR", cfsr, f[6], addr, addr_valid);
    // Write-1-to-clear and sticky: a bit left set mislabels the NEXT thread's fault.
    kickos::arm::reg32(SCB_CFSR) = cfsr;
    kickos::arm::reg32(SCB_HFSR) = hfsr;

    kickos_arm_fault_resume_at(f, &kickos_armv7m_fault_stack_reset);
    // Keep T (bit 24) and the stack-realign bit 9, which belongs to the pop at the original
    // SP; clear IT/ICI (bits 26:25 and 15:10): a fault inside an IT block would otherwise
    // resume with stale condition state and conditionally skip the stub's first instructions.
    f[7] = (f[7] & ~((3u << 25) | (0x3Fu << 10))) | (1u << 24);
    kickos_arm_fault_resume_privileged();
```

Step by step:

- **The record.** `kickos_fault_record` stores the status register's name and value, the
  faulting PC, and the faulting address when the core latched a valid one (MMFAR for an MPU
  fault, BFAR for a bus fault; neither for this one). There is one record per core, and it
  remembers which thread it belongs to (`FaultRecord::owner`), because the window before it is
  printed can be preempted and another thread's fault could overwrite it.
- **Clear the status bits.** CFSR and HFSR are sticky and cleared by writing 1s. Left set, they
  would label the next thread's fault with this one's cause.
- **Rewrite the frame.** `kickos_arm_fault_resume_at` puts the address of a small stub,
  `kickos_armv7m_fault_stack_reset`, in the stacked PC, and the top of the thread's kernel
  stack (`kickos_fault_stack_top`, which answers `ctx.kernel_sp`) in the stacked r0. The
  frame stays where the hardware put it, because the hardware will pop it from there.
- **Clear the IT bits** of the stacked xPSR, so the stub does not resume inside a half-run
  conditional block.
- **Become privileged on return.** Exception return restores the frame but not `CONTROL`.
  Clearing `CONTROL.nPRIV` now means the thread comes back from the exception privileged.

**Try it:**

```
(gdb) break kickos_fault_record
(gdb) continue
Breakpoint 1, kickos_fault_record (status_name=0xd915 "CFSR", status=65536, pc=264, addr=0, addr_valid=0) at .../kernel/init/fault.cc:53
(gdb) break kickos_arm_fault_resume_privileged
(gdb) continue
(gdb) x/8wx $psp
0x20029fd8:	0x20000c30	0x00000000	0x00000001	0x00000003
0x20029fe8:	0x0000000a	0x000014bd	0x00001aa6	0x61000000
(gdb) info symbol 0x1aa6
kickos_armv7m_fault_stack_reset in section .text
(gdb) p/x $k->currents.cells[0]->ctx.kernel_sp
$1 = 0x20000c30
```

Compare with Stop 2: the first word (r0) went from `0x1b` to `0x20000c30`, the thread's
kernel stack top, and the seventh (PC) from `0x108` to `0x1aa6`, the stub.

**Why, and the contract:** [Reference: invariants, `fault-kill-path-never-enters-panic` and `fault-record-is-printed-only-by-its-owner`](../reference/invariants.md),
[Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond)
("The two seams", "The handler MUST NOT PRINT")

### Stop 5: the dying thread walks into its own exit
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `kickos_armv7m_fault_stack_reset`; `kernel/init/fault.cc`,
`kickos_thread_fault_exit`

`kickos_fault_kill_thread` returns true, the reporter returns, and that return is the
exception return. The core pops the rewritten frame and resumes in thread mode, privileged,
on the PSP, at the stub:

```
__attribute__((naked, noreturn)) void kickos_armv7m_fault_stack_reset(void)
{
    __asm volatile("mov sp, r0\n\t"
                   "b   kickos_thread_fault_exit");
}
```

Its first instruction moves SP to r0, the top of the thread's private kernel stack. From now
on nothing runs on the user stack. That matters: another thread of the same task shares the
domain and could overwrite frames on the user stack while the exit path walks the kernel's
data structures. Then it branches into `kickos_thread_fault_exit`, which runs as the dying
thread, in thread context, with interrupts enabled. This is an ordinary kernel path now, not
an exception handler, and it may print. It prints the record through `kprintf_fault` (a path
that still reaches a user console driver if one owns stdout, see
[Reference: console, "A fault record while a driver owns the console"](../reference/console.md#a-fault-record-while-a-driver-owns-the-console)),
then exits:

```
    ::kickos::krecord_end();
    ::kickos::sched::exit_current(KOS_EXIT_FAULT, ::kickos::sched::EXIT_FAULTED);
```

`KOS_EXIT_FAULT` is 139. It is the exit status of the task, which the task's creator can read
later (`kos_task_exit_status`), and of the whole program if this was the last thread.

**Try it:**

```
(gdb) break kickos_armv7m_fault_stack_reset
(gdb) continue
(gdb) info registers r0 sp control xpsr
r0             0x20000c30          536874032
sp             0x20029ff8          0x20029ff8
control        0x2                 2
xpsr           0x61000000          1627389952
(gdb) break kickos_thread_fault_exit
(gdb) continue
(gdb) info registers sp control
sp             0x20000c30          0x20000c30 <kickos::(anonymous namespace)::g_kstacks+3024>
control        0x2                 2
```

At the stub: `CONTROL = 2` means SPSEL is set (thread mode uses the PSP) and nPRIV is clear
(privileged). The exception number in xPSR is 0, thread mode. SP is `0x20029ff8`, just above
where the frame was (`0x20029fd8` + 32): the frame has been popped. One instruction later SP is
the kernel block top, inside `g_kstacks`, the array of per-thread kernel stacks.

**Why, and the contract:** [Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond),
[Reference: invariants, `fault-exit-code-is-not-a-joiner-signal`](../reference/invariants.md)

### Stop 6: exit, part one: mark it dying, end the task
**Where:** `kernel/sched/sched.cc`, `sched::exit_current`; `kernel/task/task.cc`, `task_end`,
`mark_ended`, `task_release`; `kernel/thread/park.cc`, `task_cancel_group`

`exit_current` is the one exit path. A thread that returns from its function, one that calls
`kos_exit`, one that is slain, and one that faulted all end here. The fault reaches it with
`cause = EXIT_FAULTED`. The first block runs under `IrqLock`:

- `c->dying = true`. From here a second fault panics (Stop 3).
- `notify_unbind_self`: if the thread was bound to a notification, unbind it, so no wake can
  land on a thread already in teardown.
- **End the task.**

```
                // A fault ends the group (task_end), and so does the entry's death by any cause.
                // A cancelled thread latches nothing: the task keeps KOS_EXIT_CANCELLED.
                if (cause == EXIT_FAULTED or c->task_entry)
                {
                    task_end(c->task, code, c->cancel_kind == CANCEL_NONE, lock);
                }
```

  `task_end` calls `mark_ended`, which sets the task's `ENDED` mark, stores the exit status
  (139), and raises the task's watch if anyone set one (Stop 9). Then, if the task has other
  members, `task_cancel_group` slays every one of them. A faulting thread takes the rest of
  its task with it. `faulter` is alone in its task, so nobody else is cancelled here.
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
(gdb) p $c->task_entry
$4 = true
(gdb) break kickos::task_end
(gdb) continue
(gdb) set $t = t
(gdb) p $t->refcount
$5 = 1 '\001'
(gdb) p $t->marks
$6 = 0 '\000'
(gdb) finish
(gdb) p $t->marks
$7 = 8 '\b'
(gdb) p $t->exit_status
$8 = 139
(gdb) p $c->dying
$9 = true
```

`task_entry` is true because `faulter` was the first thread seated in its task. Mark 8 is
`KOS_TASK_ENDED` (`user/include/kickos/sys/abi.h`).

**Why, and the contract:** [Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond)
("The rule"), [Reference: invariants, `fault-exit-code-is-not-a-joiner-signal`](../reference/invariants.md)

### Stop 7: exit, part two: release every capability
**Where:** `kernel/syscall/cap.cc`, `cap_teardown`, `teardown_entry`, `obj_close_protocol`, `obj_ref_drop`;
`kernel/sync/sync.cc`, `mutex_force_unlock`

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

What `teardown = true` changes, type by type, in `obj_close_protocol`:

- **A mutex the thread owns.** A voluntary close of an owned mutex is refused. On teardown the
  kernel force-unlocks it instead (`mutex_force_unlock`): the highest waiter gets the mutex
  and its `kos_mutex_lock` returns `-KOS_EOWNERDEAD`, so it knows the protected data may be
  inconsistent. Nobody is left waiting on a dead owner.
  [4. A mutex, Stop 12](04-mutex.md#stop-12-when-a-waiter-or-an-owner-goes-away) walks the waiter
  side.
- **An endpoint.** As on a close: if this was the last capability that could receive, every
  sender parked on it is answered at once.
- **A reply capability.** If the thread was a server holding a caller's reply capability, the
  caller is woken with `-KOS_EPIPE` ([Call and reply](05-call-and-reply.md)).
- **An IRQ capability.** These are released first, all in one locked pass, so a supervisor
  woken by this death can claim the line again at once.

The rest is swept `KCAP_TEARDOWN_CHUNK` (4) slots at a time, with the lock dropped between
chunks: a big table must not keep interrupts masked for long. And `obj_ref_drop` with
`teardown = true` follows one more rule: if this was the last reference but a waiter is still
parked on the object, the object is **leaked** (kept with one reference) rather than freed
under the waiter.

**Try it:** `faulter` holds one capability: slot 1, the C library lock it shares with every
thread of the address space.

```
(gdb) break kickos::cap_teardown
(gdb) continue
(gdb) set $c = c
(gdb) p $c->caps.chunk[0][1]
$1 = {obj = 0, type = 2 '\002', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0}
(gdb) p/d $k->mutex_refs[0]@3
$2 = {3, 0, 0}
(gdb) finish
(gdb) p $c->caps.chunk[0][1]
$3 = {obj = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 1}
(gdb) p/d $k->mutex_refs[0]@3
$4 = {2, 0, 0}
```

The lock had three holders (root, `main`, `faulter`) and has two now. The slot is empty, with
a new generation.

To see a mutex force-unlocked, use the `selftest` image. Its test `t_mutex_owner_died`
(`user/apps/common/selftest/main.cc`) has a thread `odOwn` exit while it holds a mutex that
`odWt` waits for. That is an ordinary exit, not a fault, but the teardown is the same code.
Chapter 4 showed the waiter's side of this run; here the backtrace shows the teardown that
reached it:

```
(gdb) break kickos::mutex_force_unlock
(gdb) continue
Breakpoint 1, kickos::mutex_force_unlock (m=0x20004a1c <...>, dying=0x20004fe0 <...>, held=...) at .../kernel/sync/sync.cc:463
(gdb) p dying->name
$1 = "odOwn\000\000\000\000\000\000\000\000\000\000"
(gdb) p dying->dying
$2 = true
(gdb) next
(gdb) next
(gdb) p w->name
$4 = "odWt", '\000' <repeats 11 times>
(gdb) bt 5
#0  kickos::mutex_force_unlock (...)
#1  kickos::(anonymous namespace)::obj_close_protocol (closer=..., e=..., teardown=teardown@entry=true, held=...)
#2  kickos::(anonymous namespace)::teardown_entry (c=..., i=i@entry=2, held=...)
#3  kickos::cap_teardown (c=...)
#4  kickos::sched::exit_current (code=0, cause=<optimized out>, held=0x0)
```

**Why, and the contract:** [Book: Adding a kernel object type](../book/adding-a-kernel-object-type-the-additive-recipe.md)
(the close protocol and the teardown flag), [Book: Priority inheritance](../book/priority-inheritance-lending-urgency.md)
(owner-died), [Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
("Lifecycle")

### Stop 8: exit, part three: wake who waits, report the death, switch away
**Where:** `kernel/sched/sched.cc`, `sched::exit_current`; `kernel/thread/park.cc`, `thread_abort_park`;
`kernel/task/task.cc`, `task_sweep_done`, `task_report_death`; `kernel/include/kickos/thread.h`,
`ThreadPool::alloc`

The last block of `exit_current`, under the lock again:

- The thread's state becomes **EXITED**. Its pool slot may now be reused; its memory comes
  back later: the slot is reclaimed by the next `ThreadPool::alloc`, which only then pushes a
  kernel-owned stack back on the free list ([Spawn and capabilities, Stop 7](08-spawn-and-caps.md))
  and returns the capability run to the slab, because only then is the thread provably off
  the CPU.
- `task_sweep_done`: this member's sweep is finished. If it was the last member and the last
  sweep, the task is **dead**.
- Every thread parked waiting for this thread (in `kos_thread_join` or `kos_thread_slay`) and,
  if the task is dead, every thread parked waiting for the task to empty (`kos_task_slay`) is
  woken (`thread_abort_park`) with result 0.
  A joiner learns that the thread is gone, not how it died; the status is the creator's to
  read from the task.
- `task_report_death` sets the task's `DEAD` and `ENDED` marks and raises its watch.
- If no thread is left at all, `kickos_terminate` ends the system with this exit code.
  Otherwise `reschedule` switches to someone else, and this thread never runs again.

**Try it:**

```
(gdb) break kickos::thread_abort_park
(gdb) break kickos::task_report_death
(gdb) continue
(gdb) p t->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p t->wait_kind
$2 = kickos::WAIT_JOIN
(gdb) p result
$3 = 0
(gdb) continue
(gdb) set $t = t
(gdb) p $t->marks
$4 = 8 '\b'
(gdb) p $t->refcount
$5 = 0 '\000'
(gdb) finish
(gdb) p $t->marks
$6 = 12 '\f'
```

`main` was parked in its join and is woken with 0. The task goes from `ENDED` (8) to
`DEAD | ENDED` (12).

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
(the park and the wake), [Reference: invariants, `fault-exit-code-is-not-a-joiner-signal`](../reference/invariants.md)

### Stop 9: whoever watches is told
**Where:** `kernel/task/task.cc`, `task_watch_call`, `mark_ended`, `watch_raise`, `task_state_call`,
`task_exit_status_call`; `system/init/compose/walk.cc`, `Walk::wake`, `Walk::death`, `Walk::end`

`faulter`'s task had no watch: `main` waited by joining. The init, though, watches every task
it starts ([Spawn and capabilities, Stop 4](08-spawn-and-caps.md)): `kos_task_watch` stores a
notification and a bit in the task, and `watch_raise` sets that bit whenever the task ends,
dies, or becomes ready. The init sleeps in `kos_notify_wait`, and the bit wakes it.

`faulter`'s death does not touch the init. To see the watch fire, use `sysdefault_fault`
(`user/apps/common/sysdefault/fault.c`), where it is `main` itself that executes the undefined
instruction. Everything up to here happens the same way, to `main`. Then the init wakes and
asks the kernel what happened:

```
            int const state = kos_task_state(r.handle);
            // ...
            bool const ended = (state & KOS_TASK_ENDED) != 0;
            if (i == ends_ and ended)
            {
                ending = true;
                continue;
            }
            if ((state & KOS_TASK_DEAD) != 0)
            {
                check(kos_task_kill(r.handle), "kos_task_kill", i);
                report(Report::Kind::RELEASED, i, nullptr, 0);
                death(i);
                continue;
            }
```

The bit says only where to look; the task's state says what happened. `main` is the task the
composition's `ends:` names, so its end is the end of the system: the init reads its exit
status (`kos_task_exit_status`, 139) and calls `Walk::end`, which shuts down with that status.
A task that is not `ends:` and is found `DEAD` is instead released with `kos_task_kill`, and
`Walk::death` decides from its `restart_max` whether to start it again. A supervisor written
by hand does the same with its own notification.

**Try it:** on `sysdefault_fault`:

```
(gdb) break kickos::task_end
(gdb) continue
(gdb) p t->watch_notify
$1 = 1
(gdb) p t->watch_bit
$2 = 0 '\000'
(gdb) break kickos::notify_raise
(gdb) continue
(gdb) bt 6
#0  kickos::notify_raise (n=0x20006ee8 <...>, bit=1) at .../kernel/notify/notify.cc:173
#1  kickos::(anonymous namespace)::watch_raise (t=...) at .../kernel/task/task.cc:99
#2  kickos::(anonymous namespace)::mark_ended (t=..., held=..., latch=<optimized out>, code=139) at .../kernel/task/task.cc:130
#3  kickos::(anonymous namespace)::mark_ended (...)
#4  kickos::task_end (t=..., code=code@entry=139, ...) at .../kernel/task/task.cc:366
#5  kickos::sched::exit_current (code=code@entry=139, cause=cause@entry=kickos::sched::EXIT_FAULTED, held=held@entry=0x0) at .../kernel/sched/sched.cc:933
(gdb) delete
(gdb) break kickos::task_state_call
(gdb) continue
(gdb) p $k->currents.cells[0]->name
$3 = "root", '\000' <repeats 11 times>
(gdb) finish
Value returned is $4 = 12
(gdb) break kickos::task_exit_status_call
(gdb) continue
(gdb) finish
Value returned is $5 = 0
(gdb) p status
$6 = 139
(gdb) continue
[Inferior 1 (process 1) exited with code 0213]
```

`bit=1` is `1u << watch_bit`, bit 0. The init (running as root) reads state 12, `DEAD | ENDED`,
and status 139. gdb prints the exit code in octal: `0213` is 139.

**Why, and the contract:** [Reference: invariants, "App entry & init seam"](../reference/invariants.md#app-entry--init-seam),
[Reference: invariants, `fault-exit-code-is-not-a-joiner-signal`](../reference/invariants.md)

### Stop 10: the survivor runs on
**Where:** `user/apps/common/faultsurvive/main.cc`, `main`; `kernel/syscall/syscall_thread.cc`, `task_kill`;
`kernel/task/task.cc`, `task_stop`, `task_drop_hold`, `free_task`

Back on `faultsurvive`. `main`'s join returns 0. `main` then calls `kos_task_kill(victim)`.
The task is already dead, so there is nobody to cancel (`task_stop` finds no member). What
remains is the **creator's hold**: a task that its creator still names keeps its slot even
with no member, so the creator can read its state and spawn into it again. `task_drop_hold`
gives that up, and with no member left, `free_task` releases the domain and bumps the slot's
generation, so the old handle `4` stops resolving. Then `main` prints its line and returns 0,
which ends the system.

The task is not a capability, so this is not a `handle_close`. It is the creator's authority,
checked by `task_created_by`.

**Try it:**

```
(gdb) break kickos::task_kill
(gdb) continue
(gdb) p $k->currents.cells[0]->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p $k->currents.cells[0]->wait_result
$2 = 0
(gdb) p/x task
$3 = 0x4
(gdb) p $k->tasks[3]
$4 = {domain = 0x20006428 <...>, refcount = 0 '\000', creator_tag = 2 '\002', gen = 0, prio_ceiling = 31 '\037',
  marks = 12 '\f', ...}
(gdb) finish
Value returned is $5 = 0
(gdb) p $k->tasks[3]
$6 = {domain = 0x0, refcount = 0 '\000', creator_tag = 0 '\000', gen = 1, prio_ceiling = 31 '\037',
  marks = 12 '\f', watch_bit = 0 '\000', sweeping = 0 '\000', watch_notify = 0, watch_ep = 0, exit_status = 139}
(gdb) p $k->threads.slots[2].state
$7 = {v_ = kickos::ThreadState::EXITED}
(gdb) continue
[Inferior 1 (process 1) exited normally]
```

`wait_result` 0 is what the join returned. After the kill, the slot has no domain and no
creator, and its generation is 1. `faulter`'s pool slot (2) is EXITED, waiting for the next
spawn to reclaim it. The program exits with status 0.

**Why, and the contract:** [Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond),
[Reference: architecture, "Memory domains (the isolation unit)"](../reference/architecture.md#memory-domains-the-isolation-unit)

## The other road: a fault the kernel does not contain, and the kernel's own panic

Everything above depends on Stop 3 saying yes. When it says no, the same reporter takes the
other branch, and the system stops.

### Stop 11: a fault that panics
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `kickos_armv7m_fault_report`; `kernel/init/console.cc`,
`kpanic_enter`; `arch/common/kfault_terminate_shutdown.cc` and `arch/common/kfault_terminate_default.cc`,
`kfault_terminate`

`faultsurvive_ovf` (the same source with `KICKOS_FS_MODE=1`) makes `faulter` recurse until it
runs off the bottom of its stack. The MPU forbids the write below the stack, so the core
faults while pushing the exception frame itself. CFSR gets `MSTKERR` (a MemManage fault during
stacking), and Stop 3's second test refuses: there is no frame to trust, so there is nothing
to rewrite and nowhere safe to resume. The reporter goes on:

- `kpanic_enter` masks interrupts for good, takes the console back from any user driver, and
  flushes whatever is queued.
- It prints the dump: a label from CFSR (`MPU FAULT`, `BUS FAULT` or `HARD FAULT`), the frame
  if it could be read, CFSR and HFSR, and MMFAR or BFAR when valid.
- `kfault_terminate` ends everything. On QEMU the chip opts into
  `arch/common/kfault_terminate_shutdown.cc`, which exits the emulator with status 132. On the
  f411 the fallback `arch/common/kfault_terminate_default.cc` blinks the diagnostic LED forever.

```
[fs] worker about to fault

=== MPU FAULT ===
  frame not read: its stacking or unstacking at 0x20027fe0 (PSP) faulted
  CFSR=0x92 HFSR=0x0
  MMFAR=0x20027ff8
```

`0x92` is `MSTKERR` (bit 4) and `MMARVALID` (bit 7). MMFAR names the address just below
`faulter`'s stack where the push was refused.

**Try it:** on `faultsurvive_ovf`:

```
(gdb) break kickos_armv7m_fault_report
(gdb) break arch_fault_is_user_thread
(gdb) break kpanic_enter
(gdb) break kfault_terminate
(gdb) continue
(gdb) p/x exc_return
$1 = 0xfffffffd
(gdb) p/x *(unsigned*)0xE000ED28
$2 = 0x92
(gdb) continue
(gdb) finish
Value returned is $3 = false
(gdb) continue
Breakpoint 3, kpanic_enter () at .../kernel/init/console.cc:904
(gdb) bt 3
#0  kpanic_enter () at .../kernel/init/console.cc:904
#1  0x00001cbc in kickos_armv7m_fault_report (frame=0x20027fe0, exc_return=4294967293) at .../arch/arm/armv7m/arch_armv7m.cc:412
#2  <signal handler called>
(gdb) continue
Breakpoint 4, kfault_terminate () at .../arch/common/kfault_terminate_shutdown.cc:21
```

(gdb shows the exception entry as `<signal handler called>`.)

**Why, and the contract:** [Reference: porting, "Fault-reporter contract"](../reference/porting.md#fault-reporter-contract-panic-must-survive-console-handover),
[Reference: invariants, `fault-frame-untrusted-until-proved`](../reference/invariants.md)

### Stop 12: the kernel's own panic
**Where:** `kernel/include/kickos/kernel.h`, `KICKOS_ASSERT`; `kernel/init/console.cc`, `kpanic`,
`kickos_panic_report`; `arch/arm/armv7m/switch.S`, `kickos_panic_stack_enter`

A fault is the hardware's verdict. A **panic** is the kernel's own: an invariant that cannot
hold any more. `KICKOS_ASSERT(cond)` calls `kpanic` when `cond` is false (`kpanic_at` in a
terse diagnostic build), and a user thread can
ask for the same end with `kos_panic` (the `KOS_SYS_PANIC` arm, `user_panic`, which copies the
message in with the same per-byte checks as any user string). `kpanic` does not trust the
stack it was called on: `kickos_panic_stack_enter` (assembly) moves SP to a dedicated panic
stack (`g_panic_stack`, one per core) and calls `kickos_panic_report`, which calls
`kpanic_enter` and prints `KERNEL PANIC:` with the message. Then `kfault_terminate`, as above.

There is no kill and no survivor on this road. The panic path is for the case where the kernel
can no longer vouch for its own state; containing a fault is for the case where it still can.

**Try it:** on `panicgate1` (`user/apps/common/panicgate/main.cc`, case 1), whose `main` calls
`kos_panic`:

```
(gdb) break kickos_panic_report
(gdb) break kfault_terminate
(gdb) continue
Breakpoint 1, kickos_panic_report (msg=0x200007b8 <...> "[panicgate] message on the wire", file=0x0, line=0) at .../kernel/init/console.cc:830
(gdb) bt 2
#0  kickos_panic_report (msg=... "[panicgate] message on the wire", file=0x0, line=0) at .../kernel/init/console.cc:830
#1  0x00004a64 in kickos::kpanic (msg=<optimized out>) at .../kernel/init/console.cc:772
(gdb) info registers sp
sp             0x20004520          0x20004520 <kickos::g_kstack_report>
(gdb) continue
Breakpoint 2, kfault_terminate () at .../arch/common/kfault_terminate_shutdown.cc:21
```

`0x20004520` is the top of `g_panic_stack` (it starts at `0x20004370` and is `0x1b0` bytes;
gdb names the next symbol). The console shows:

```
KERNEL PANIC: [panicgate] message on the wire
KSTACK HIGH WATER: 344 of 1004 bytes reached on this slot
```

and QEMU exits with 132.

**Why, and the contract:** [Reference: porting, "Fault-reporter contract"](../reference/porting.md#fault-reporter-contract-panic-must-survive-console-handover),
[Reference: invariants, `panic-console-probe-independent`](../reference/invariants.md)

## What you have seen

An unprivileged thread's fault arrives as a hardware exception with a frame on the thread's
own stack. Before anything is printed, the kernel decides from the CPU's privilege, the fault
status bits, the frame's position and the thread's state whether the fault is the thread's.
If it is, the handler records the facts and rewrites the frame so that the exception return
resumes the dying thread, privileged, in a stub that moves to its kernel stack and calls the
one exit path. That path ends the task, closes every capability (force-unlocking mutexes,
answering parked callers and senders), wakes joiners, marks the task dead, and raises the
watch. The creator learns the exit status from the task; the init, if it watches, restarts or
ends. The system keeps running. If any test fails, the same handler panics instead.

## Where to next

- [10. The MMU](10-the-mmu.md): the next chapter, the same walk on a part with page tables instead
  of an MPU.
- [8. Spawn and capabilities](08-spawn-and-caps.md): the forward direction of everything this chapter
  undoes.
- [4. Mutex](04-mutex.md): the waiter that receives `-KOS_EOWNERDEAD`.
- [5. Call and reply](05-call-and-reply.md): the caller that receives `-KOS_EPIPE` when its
  server dies.
- [12. ARMv6-M](12-armv6m.md): the same stops on a core with no CFSR, where only the frame
  bounds test can refuse.
- [14. RISC-V](14-riscv.md): the same decision read from `mstatus.MPP`, with the frame built on
  the kernel stack by software.

## Exercises

1. **A fault in the caller's task.** In `faultsurvive`, pass `KOS_TASK_NONE` instead of
   `victim`, so `faulter` joins `main`'s task. Predict who prints after the fault, and the QEMU
   exit status. Check with `break kickos::task_cancel_group` and `p t->refcount` (it is now
   more than one).
2. **An MPU fault with an address.** Run `mpu_fault` (`user/apps/common/mpu_fault/main.cc`),
   whose thread writes just past its granted region. Predict the CFSR bits and whether
   `kickos_fault_record` gets `addr_valid = 1`. Check at `break kickos_fault_record`, and
   compare the address with the `[domain] expect fault at` line the app prints.
3. **A fault during the exit.** Reason through what Stop 3's `dying` test does if a thread
   faults again inside `cap_teardown`. Which branch of `kickos_armv7m_fault_report` runs, and
   why is that safer than a second redirect?
