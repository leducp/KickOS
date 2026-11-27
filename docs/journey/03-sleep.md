<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 3. Sleep: the time base, the one-shot timer, the park, the wake

By the end of this chapter you will have followed a thread that asks to sleep for 0.4 s. You will
see where "now" comes from, how the kernel turns "0.4 s from now" into one absolute deadline, how
it parks the thread so the scheduler stops choosing it, how it programs a hardware timer to fire
exactly once at that deadline (no periodic tick), what the CPU runs while nobody has work, and how
the timer interrupt wakes the thread so that it resumes in the middle of its syscall and returns
to user code.

The ideas are in three Book chapters: [The time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md)
(what a monotonic clock and a one-shot timer are, and why the kernel is *tickless*: it programs the
timer for the next event instead of taking an interrupt every millisecond),
[The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md) (how every
blocking call parks and is woken), and [Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md)
(why a switch on Cortex-M is deferred). This chapter shows the code. It builds on
[the first syscall](02-first-syscall.md): the trap, the trampoline and the way back are the same
here and are not repeated.

## The image

We use `hello` again (`user/apps/common/hello/main.cc`) on the `qemu` build. After `main` has
printed, it starts two threads at priority 10, `ping` and `pong`, and waits on a third
semaphore, `g_done`, which `pong` posts after the last round. The two threads hand a token back
and forth for ten rounds:

```
    void ping(void*)
    {
        for (int n = 1; n <= ROUNDS; n++)
        {
            g_ping.wait();
            kos::sleep_for(std::chrono::nanoseconds{BEAT_NS});
            say("ping", n);
            g_pong.post();
        }
    }
```

`BEAT_NS` is 400000000, 0.4 s. `g_ping` and `g_pong` are two semaphores in the app's static
data, `g_ping` starting at 1, so `ping` gets the token first and the first sleep in the image is
`ping`'s. Boot it under gdb as in [Setup](00-setup.md):

```
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/hello/hello -gdb tcp::1234 -S &
gdb-multiarch build/qemu/user/apps/common/hello/hello \
    -ex 'target remote :1234'
```

Timing note: the `qemu` clock is an emulated timer, and QEMU stops it while gdb holds the target,
so a pause at a breakpoint does not move a deadline closer. The numbers below are from real runs;
yours will differ by a fraction of a millisecond, and a function gdb runs on the target (`p
kickos::ktime_now()`) costs a little emulated time.

### Stop 1: the app asks to sleep
**Where:** `user/include/kickos/kos.h`, `kos::sleep_for`; `user/src/syscall_stubs.cc`, `kos_sleep_for`,
`sleep_limit`; `user/include/kickos/sys/abi.h`, `kos_limit_word`, `enum kos_limit_kind`

`kos::sleep_for` turns the `std::chrono` duration into a `kos_duration_t` of nanoseconds and calls
`kos_sleep_for`, which passes it on with a *kind*:

```
static void sleep_limit(int64_t ns, uint32_t kind)
{
    uint64_t const limit = kos_limit_word(ns);
    arch_syscall(KOS_SYS_SLEEP, kos_u64_lo(limit), kos_u64_hi(limit), kind, 0, 0);
}

void kos_sleep_for(kos_duration_t d)
{
    sleep_limit(d.ns, KOS_LIMIT_RELATIVE);
}
```

Every timed call in KickOS carries its time the same way: one 64-bit *limit word* and a kind
that says how to read it. `KOS_LIMIT_MONOTONIC` is a deadline on the monotonic clock,
`KOS_LIMIT_REALTIME` a deadline on the wall clock, and `KOS_LIMIT_RELATIVE`, a duration from the
kernel's own clock reading at entry, is taken by a sleep alone. `kos_limit_word` maps the
"forever" value to `KOS_LIMIT_NONE` and a negative count to 0.

The word is 64 bits but every syscall argument is one register, 32 bits on this core. So the
stub splits it into a low and a high word with `kos_u64_lo` and `kos_u64_hi` (from
`user/include/kickos/sys/abi.h`), and the kernel joins them back. The split is done the same way on
64-bit targets too, so the register layout of every syscall is identical across the fleet.
`KOS_SYS_SLEEP` is 3. The stub ignores the result: a sleep has nothing to report.

**Try it:** `break kos_sleep_for`, `continue`, `bt 3`:

```
#0  kos_sleep_for (d=...) at .../user/src/syscall_stubs.cc:58
#1  0x000117ee in kos::sleep_for<long long, std::ratio<1ll, 1000000000ll> > (d=...) at .../user/include/kickos/kos.h:199
#2  (anonymous namespace)::ping () at .../user/apps/common/hello/main.cc:31
```

Then `break arch_syscall`, `continue`, `info registers r0 r1 r2 r3`:

```
r0             0x3                 3
r1             0x17d78400          400000000
r2             0x0                 0
r3             0x2                 2
```

400000000 fits in 32 bits, so the high word is 0. `r3` is the kind, 2, `KOS_LIMIT_RELATIVE`.

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("A 64-bit argument is split into halves, deliberately")

### Stop 2: into the kernel
**Where:** `kernel/syscall/syscall.cc`, the `KOS_SYS_SLEEP` case; `kernel/time/time.cc`, `ktime_sleep`

The trap, `SVC_Handler`, `svc_trampoline` and `syscall_dispatch` run exactly as in
[the first syscall](02-first-syscall.md), Stops 3 to 6. The sleep case joins the word and hands it
on with the kind:

```
        case KOS_SYS_SLEEP:
        {
            return static_cast<uint64_t>(ktime_sleep(
                kos_u64_join(static_cast<uint32_t>(a0), static_cast<uint32_t>(a1)), a2));
        }
```

There is no pointer and no handle to check: a limit is a plain number, and any value is allowed.
`ktime_sleep` answers `-KOS_EINVAL` for an unknown kind and 0 otherwise, whatever happens while
the thread sleeps. Keep that in mind for the last stop.

`ktime_sleep` handles the edge values before it parks anything. A relative 0 means "let someone
else run", so it calls `sched::yield()` and returns. A huge duration could overflow when added
to "now" and wrap to a time in the past, so the sum saturates at `KOS_LIMIT_NONE`, "never". A
monotonic or realtime deadline that the clock has already read also yields and never parks
(Stop 7).

**Try it:** `break kickos::ktime_sleep`, `continue`, `p kickos::sched::current()->name`:

```
Breakpoint 3, kickos::ktime_sleep (limit=400000000, kind=kind@entry=2) at .../kernel/time/time.cc:276
$1 = "ping", '\000' <repeats 11 times>
```

**Why, and the contract:** [Reference: invariants](../reference/invariants.md)
(`timer-delta-clamp-before-convert`, which also covers the saturation)

### Stop 3: where "now" comes from
**Where:** `kernel/time/time.cc`, `ktime_now`; `arch/arm/chip/mps2/chip_mps2.cc`, `arch_clock_now`,
`dualtimer_wrap_ops`, `kickos_mps2_clock_isr`; `arch/include/kickos/arch/wrap_extend.h`,
`arch_wrap_extend`

`ktime_now` is one line: `return arch_clock_now();`. The kernel's time base is a *monotonic*
clock in nanoseconds since boot: it never goes backwards and never jumps. Every deadline in the
kernel is an absolute value on this clock.

`arch_clock_now` is a *seam*: a function the kernel calls and each chip must provide. There is
deliberately no default on ARM. The only wide counter inside the Cortex-M core (the DWT cycle
counter) sits in the debug block and is not a trustworthy clock, so a chip that forgets to provide
one fails to link rather than inheriting a bad clock. The Book chapter walks through why.

On the `qemu` board the clock is Timer 1 of the board's dual timer, a 32-bit counter that counts
down at the board's 25 MHz system clock (the chip file `platform/mps2/chip.yaml` names the rate,
and the build turns it into `KICKOS_CHIP_TIMEBASE_HZ`). `arch_init` starts it with a reload of
`0xFFFFFFFF`. The chip file reads it through three small functions:

```
    struct dualtimer_wrap_ops
    {
        static uint32_t count() { return ~r32(dt::TIMER1VALUE); }
        static bool pending() { return (r32(dt::TIMER1RIS) & dt::RIS_INT) != 0; }
        static bool take()
        {
            if (not pending())
            {
                return false;
            }
            r32(dt::TIMER1INTCLR) = 1u;
            return true;
        }
    };
    kickos::arch_wrap_extend<dualtimer_wrap_ops> g_clk_wrap;
```

The complement turns the down-counter into one that counts up. 32 bits at 25 MHz wrap every
172 s, so `arch_wrap_extend` keeps a high word in software. The timer's wrap interrupt, line 10,
goes to `kickos_mps2_clock_isr` (the one vector `startup.S` does not give to the generic entry),
which moves the high word on. A reader that finds the wrap pending but not yet taken adds it
itself, so the 64-bit value never goes back. `arch_clock_now` is then `g_clk_wrap.read()` times
40, the nanoseconds of one tick at 25 MHz.

**On the silicon twin**, `arch/arm/chip/stm32f411/chip_stm32f411.cc` reads TIM2, a 32-bit
peripheral timer counting at the core clock, extends it to 64 bits with the same
`arch_wrap_extend` (`tim2_wrap_ops`, its overflow interrupt `kickos_tim2_clock_isr`), and converts
ticks to nanoseconds through an anchor (`g_clk`, `arch/include/kickos/arch/clk_anchor.h`), since
its rate changes when the core clock does. Same seam, same promise.

**Try it:** at the `ktime_sleep` stop, `p kickos::ktime_now()`:

```
$2 = 29559200
```

About 29.6 ms since boot, a multiple of 40 ns. (gdb runs the function on the target to answer
this, which is fine here because reading the clock writes nothing.)

**Why, and the contract:** [Book: the time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md)
("The seam", "The worked example: a cycle counter is the wrong authority"),
[Reference: architecture](../reference/architecture.md) (`arch_clock_now`, in "The porting layer")

### Stop 4: one absolute deadline, and the park
**Where:** `kernel/time/time.cc`, `ktime_sleep`, `sleep_park`, `ktime_deadline_arm_at`;
`kernel/include/kickos/time.h`, `ktime_floored`

For a relative limit `ktime_sleep` adds the duration to its clock reading and calls
`sleep_park(deadline, false)`. From here on the kernel only ever talks about the absolute
deadline, never "0.4 s". This is the whole function:

```
        void sleep_park(uint64_t deadline_ns, bool realtime)
        {
            IrqLock lock;
            Thread* c = sched::current();
            // Ahead of every side effect below: sched::exit_current does not sweep the sleep
            // queue, so an exit after the insert would leave this thread on it.
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            park_queueless(ask)(c, WAIT_SLEEP, nullptr, lock);
            if (realtime)
            {
                realtime_deadline_arm(c, deadline_ns, lock);
            }
            else
            {
                ktime_deadline_arm_at(c, ktime_floored(ktime_now(), deadline_ns), false, lock);
            }
            sched::reschedule(nullptr, lock); // its switch arms the timer for the incoming thread
        }
```

Read it in order:

1. `IrqLock lock;` opens a critical section that lasts to the closing brace. Everything below is
   one step as far as interrupts and other kernel code are concerned. That is what prevents a
   *lost wakeup*, where the waker runs between "decide to sleep" and "be asleep" and finds nobody
   to wake.
2. `park_cancel_pending` returns a `ParkToken`, and asks one question first: has someone asked to
   kill this thread? If so, it dies now, before it is put anywhere. The park functions can only be
   called with that token in hand (`kernel/include/kickos/sync.h`), so no blocking path can forget
   to ask.
3. `park_queueless` marks the thread blocked and takes it off the ready lists (Stop 5).
4. `ktime_floored` makes sure the deadline is at least `KICKOS_TIMER_MIN_DELTA_NS` (20 us,
   `kernel/include/kickos/config/board.h`) ahead of now. A deadline too close to arm becomes
   "as soon as the timer can". This is where a sleeper's deadline is born, floored against one
   clock reading.
5. `ktime_deadline_arm_at` stores the deadline in the thread and puts it on the sleep queue
   (Stop 5). A realtime deadline takes `realtime_deadline_arm` instead, which converts it to the
   monotonic clock and marks it, so a later set of the wall clock can move it (Stop 7).
6. `sched::reschedule` picks someone else (Stop 6).

**Try it:** `break kickos::(anonymous namespace)::sleep_park`, `continue`:

```
Breakpoint 4, kickos::(anonymous namespace)::sleep_park (deadline_ns=429973080, realtime=false) at .../kernel/time/time.cc:252
(gdb) p kickos::sched::current()->state
$3 = {v_ = kickos::ThreadState::RUNNING}
```

About 30 ms plus 400 ms: the deadline is 429973080 ns.

**Why, and the contract:** [Book: the blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
("Why it matters: the lost wakeup"), [Reference: invariants](../reference/invariants.md)
(`timer-min-delta-guard`)

### Stop 5: parked, on no wait queue, on the sleep queue
**Where:** `kernel/sync/sync.cc`, `park_queueless` and `wq_block`; `kernel/time/time.cc`,
`sleepq_insert`; `kernel/sched/sched.cc`, `sched::detach_current`; `kernel/include/kickos/instance.h`,
`Kernel::sleepq`

Every blocking call in KickOS parks through the same few lines. `park_queueless` is the core of
them:

```
    uint32_t park_queueless(Thread* c, WaitKind kind, void* obj, Held held)
    {
        // BLOCKED before the detach: on_remove reads `state` to tell a park from a set_prio
        // re-seat, and only a park forfeits the RR slice remainder. The removal reads `link`,
        // so it must run before anything re-uses that node.
        c->state.block();
        sched::detach_current(c, held);
        c->wait_queue = nullptr;
        c->wait_kind = kind;
        c->wait_obj = obj;
        return c->switch_count;
    }
```

It sets the state to `BLOCKED`, removes the thread from the scheduler's ready lists, and records
the *wait edge*: `wait_kind` and `wait_obj` say what the thread is waiting for, so that a waker who
holds only the thread (a timer, a kill) can still find and undo the park. `sched::detach_current`
also enforces a rule: it panics if called from an interrupt handler, because there the switch
would be deferred and the "blocked" thread would keep running.

A semaphore wait with no unit to take, or a message send, calls `wq_block`, which is
`park_queueless` plus a push onto the object's wait queue (a list of the threads waiting on
that object) plus the reschedule. A
sleep has no object to wait on, so it uses `park_queueless` directly with `WAIT_SLEEP` and no
queue. Its only list is the *sleep queue*, `Kernel::sleepq(core)`, one per kernel core: a singly
linked list of every thread with a deadline on that core, sorted soonest first and linked through
`Thread::tnext`. `sleepq_insert` walks to the right place and sets `on_timer`. The sleep queue uses a different link field from the ready
lists and wait queues, so a thread can be on a wait queue and on the sleep queue at once; that is
how a wait with a timeout works ([Mutex](04-mutex.md) and [Call and reply](05-call-and-reply.md)
use it).

**Try it:** `break kickos::sched::detach_current`, `continue`, then:

```
(gdb) p kickos::sched::current()->state
$4 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) finish
kickos::park_queueless (c=c@entry=0x20002660 <kickos::(anonymous namespace)::system_pools+6944>,
    kind=kind@entry=kickos::WAIT_SLEEP, obj=obj@entry=0x0 <g_isr_vector>, held=..., held@entry=...) at .../kernel/sync/sync.cc:89
```

The state was already `BLOCKED` when the detach ran, as the comment requires. `ping`'s control block
lives at 0x20002660, in the thread pool, whose storage (`system_pools`) the build generates for
this system; keep that address for later stops.

**Why, and the contract:** [Book: the blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
("The wait edge, and why the pop retracts it"), [Reference: invariants](../reference/invariants.md)
(`no-block-from-isr`)

### Stop 6: choosing who runs next
**Where:** `kernel/sched/sched.cc`, `sched::reschedule`, `pick_and_seat`, `switch_to`, `switch_book`;
`kernel/sched/policy_fifo_rr.cc`, `policy_pick_next`

`sched::reschedule` asks the policy for the best thread and switches to it if that is not the one
already running. On one core the choice is short:

```
        KernelCore const core = kickos_kernel_core();
        int p = top_prio(kernel().ready_bitmap[core]);
        if (p < 0)
        {
            return kernel().idle(core);
        }
        return thread_of(kernel().ready[core][p].head);
```

There is one ready list per priority, and a bitmap with one bit per non-empty list. `top_prio`
finds the highest set bit, and the head of that list runs. If no list holds anyone, the answer is
the *idle* thread, which is never on a ready list and always available.

`switch_to` then calls `switch_book`, which does the bookkeeping for the incoming thread: marks it
`RUNNING` and current, bumps its `switch_count` (the counter `park_queueless` returned as an
"epoch"), installs its memory view (`aspace_activate_for`, `next->mpu.apply()`), and arms the timer
for it (Stop 7). Then it calls `arch_switch`.

**Try it:** `break arch_switch`, `continue`. The function is short and gdb stops inside its first
store, so read its two arguments from `r0` and `r1`:

```
(gdb) p ((kickos::Thread*)$r1)->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p ((kickos::Thread*)$r1)->switch_count
$2 = {v_ = {... _M_i = 2} ...}
(gdb) p $basepri
$3 = 32
```

On this first round the next thread is `main`, not `pong` and not idle. `main` created `ping`
first, and `ping`, at priority 10 above `main`'s 2, ran at once and took the token before `pong`
existed. Now `main` resumes and creates `pong`, which runs, waits on its semaphore (which `ping`
has not posted) and parks the same way. Then `main` waits on `g_done` and parks too, and the pick
after that finds every list empty and returns idle. `switch_count` is 2 because `switch_book`
has just counted this switch-in, `main`'s second. (`ctx` is the first member of `Thread`, which is
why the cast works.) `BASEPRI` is 32, 0x20: the kernel lock is still held.

**Why, and the contract:** [Book: one kernel lock, many ready queues](../book/one-lock-many-ready-queues.md),
[Reference: architecture](../reference/architecture.md) ("Scheduler (the core constraint)")

### Stop 7: programming the one-shot timer
**Where:** `kernel/time/time.cc`, `ktime_rearm`, `next_event`, `program`; `kernel/sched/sched.cc`,
`switch_book`; `arch/arm/common/arch_arm_common.cc`, `arch_timer_arm`

`switch_book` calls `ktime_rearm(next)` on the way to `arch_switch`. On this core it does so at
once for a thread with no round-robin slice to stamp, which is every thread here; a thread whose
slice must be stamped is armed where its switch lands instead (`kickos_switch_landed`, then
`ktime_land`), so the switch itself is charged to no slice. `ktime_rearm` works out the soonest
event the core must wake for: the head of this core's sleep queue, or the end of the incoming
thread's round-robin time slice if it has one.

```
            uint64_t next = UINT64_MAX;
            Thread const* const head = kernel().sleepq(kickos_kernel_core());
            if (head != nullptr)
            {
                next = head->deadline_ns;
            }
            // ...
            uint64_t const event = sched::next_timed_event(incoming);
            if (event < next)
            {
                next = event;
            }
```

```
    void ktime_rearm(Thread const* incoming, Held)
    {
        uint64_t const next = next_event(incoming);
        // ...
        if (next == kernel().timer_armed_ns[kickos_kernel_core()])
        {
            return;
        }
        program(next);
    }
```

```
        void program(uint64_t next)
        {
            kernel().timer_armed_ns[kickos_kernel_core()] = next;
            if (next == UINT64_MAX)
            {
                arch_timer_disarm();
                return;
            }
            arch_timer_arm(ktime_floored(ktime_now(), next));
        }
```

The `next == timer_armed_ns` test is important. `timer_armed_ns` is the kernel's record of what the
timer is counting toward. If nothing changed, the timer is left alone; reprogramming a countdown on
every switch would restart it each time, and a busy system could keep a far deadline from ever
being reached.

**The minimum-delta guard.** A timer programmed for a moment already gone either fires at once,
over and over, or never fires at all, depending on the hardware. So the kernel never hands
`arch_timer_arm` a time closer than `KICKOS_TIMER_MIN_DELTA_NS`:

- A deadline is floored where it is born, against one clock reading (`ktime_floored` in
  `sleep_park`, Stop 4), and `program` floors it again against a fresh reading, since the work
  between the birth and the arm can outlast the minimum delta. A deadline that is already past
  when the timer is programmed therefore fires within the minimum delta, not at once.
- A deadline that has already passed when a call starts is never armed. `ktime_limit_passed`
  checks it first: a wait answers at once, and a sleep yields (`ktime_sleep`).
- A realtime deadline is kept on the monotonic clock, marked `timer_wall`. A set of the wall
  clock moves every marked deadline by the same step, floored at now (`ktime_wall_shift`), and
  re-arms the timer.

`arch_timer_arm` is the chip-family half. On every Cortex-M it uses *SysTick*, a 24-bit
down-counter built into the core that raises an exception when it reaches zero. It converts the
time left into core clock cycles (`SystemCoreClock`, 25 MHz on the MPS2 model), clamps it to what
24 bits can hold, and loads it:

```
    reg32(SYST_CSR) = 0;                        // disable while reprogramming
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;           // drop any pend latched while masked
    reg32(SYST_RVR) = static_cast<uint32_t>(cyc);
    reg32(SYST_CVR) = 0;                        // clear -> reload on next tick
    reg32(SYST_CSR) = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
```

24 bits at 25 MHz is about 0.67 s. A longer sleep is clamped: the timer fires early, finds nobody
due, and re-arms for the rest. This is the only interrupt the sleep costs: no periodic tick runs
while `ping` sleeps.

**Try it:** `break arch_timer_arm`, `continue`, `bt 2`, `finish`, then read the three SysTick
registers (control, reload, current) at 0xE000E010:

```
Breakpoint 6, arch_timer_arm (deadline_ns=428967680) at .../arch/arm/common/arch_arm_common.cc:148
#0  arch_timer_arm (deadline_ns=428967680) at .../arch/arm/common/arch_arm_common.cc:148
#1  0x0000c414 in kickos::(anonymous namespace)::switch_book (next=0x200024e8 <...>, me=0, held=...) at .../sched.cc:414
(gdb) finish
(gdb) x/3wx 0xE000E010
0xe000e010:    0x00000007    0x00984328    0x00984049
```

Control 7 is enable, interrupt, core clock. The reload 0x984328 is 9978664 cycles: 399.15 ms at
25 MHz. The deadline is 428.97 ms, and the clock read 29.82 ms when the arm ran. The counter has
already started down.

To watch the deduplication, trace every call instead of stopping. In a fresh session, after the
`kos_sleep_for` stop:

```
break arch_timer_arm
commands
silent
printf "arm %llu\n", deadline_ns
continue
end
break kickos::ktime_rearm
commands
silent
printf "rearm for %s\n", incoming->name
continue
end
break SysTick_Handler
continue
```

prints, in one run:

```
rearm for main
arm 430986560
rearm for pong
rearm for main
rearm for idle

Breakpoint 4, SysTick_Handler () at .../arch/arm/common/arch_arm_common.cc:262
```

The switch to `main` armed the timer. The switches to `pong`, back to `main` and to idle asked
again, found the same deadline already armed, and touched nothing. Then nothing at all happened
until the timer fired.

**Why, and the contract:** [Book: the time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md)
("Tickless vs the periodic tick"), [Reference: invariants](../reference/invariants.md)
(`rearm-before-switch`, `timer-min-delta-guard`, `timer-arm-dedup-needs-disarm`,
`timer-delta-clamp-before-convert`)

### Stop 8: the switch happens when the lock opens
**Where:** `arch/arm/common/arch_arm_common.cc`, `arch_switch`; `arch/arm/armv7m/switch.S`, `PendSV_Handler`

On Cortex-M, `arch_switch` does not switch:

```
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    (void)from; // PendSV saves g_arch_current; `from` is always that thread
    g_arch_next = to;
    reg32(SCB_ICSR) = ICSR_PENDSVSET;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}
```

It records the target and sets PendSV pending. PendSV is an exception with the lowest priority,
and the kernel lock (`BASEPRI` 0x20) masks it. So `arch_switch` returns, `reschedule` returns,
and `sleep_park` reaches its closing brace, where the `IrqLock` destructor lowers `BASEPRI`
back to 0. At that instant PendSV is taken, *in the middle of `sleep_park`*, exactly as if an
interrupt had arrived there.

`PendSV_Handler` does the real switch. The hardware has already pushed the eight-word frame on the
current stack, which for `ping` is its kernel stack (the dispatch runs there, see
[the first syscall](02-first-syscall.md), Stop 5). The handler checks that stack pointer, pushes
`r4`-`r11` below the frame, saves the stack pointer into `ping`'s `arch_context.sp` and
`EXC_RETURN` into its `arch_context.exc_return`, and saves `CONTROL.nPRIV`, which is 0 because
`ping` was privileged inside its syscall on its own kernel stack. Then it loads the next thread's
saved registers and its privilege, programs its MPU regions (`kickos_arch_mpu_commit`), calls
`kickos_switch_landed` for the work that must run where a switch lands, and returns from the
exception into it.

**Try it:** `break PendSV_Handler`, `continue`:

```
Breakpoint 5, PendSV_Handler () at .../arch/arm/armv7m/switch.S:163
(gdb) p $basepri
$7 = 0
(gdb) info registers psp
psp            0x20001670          536876656
(gdb) info symbol $psp
kickos::(anonymous namespace)::system_pools + 2864 in section .bss
```

`BASEPRI` is 0: the lock has opened, which is why PendSV could run. The `PSP` is in the pool's
kernel stacks, on `ping`'s kernel stack.

**Why, and the contract:** [Book: the blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
("The hazard: a post-block read can be stale"), [Book: context switching](../book/context-switching-and-the-silicon-contract.md),
[Reference: invariants](../reference/invariants.md) (`arch-switch-may-defer`)

### Stop 9: idle
**Where:** `kernel/init/kmain.cc`, `idle_entry`; `arch/seam_defaults.cmake`, the `arch_idle_wait`
default

Once `pong` and `main` have parked too, the idle thread runs. It is a privileged kernel thread with
the lowest priority, and its body is a loop around one seam:

```
        void idle_entry(void*)
        {
            while (true)
            {
                arch_idle_wait();
            }
        }
```

The default `arch_idle_wait` (generated from `arch/seam_defaults.cmake` into the build tree)
executes `wfi`, "wait for interrupt", which stops the core until something happens. The `qemu`
chip and the F411 both run it. The dual timer that carries the clock keeps counting while the
core waits, and SysTick ends the wait.

While idle waits, `ping` is frozen. Its saved state says exactly where it will continue.

**Try it:** `break arch_idle_wait`, `continue`, `bt 2`, then look at `ping` (privileged code is
running, so gdb can read kernel memory):

```
#0  arch_idle_wait () at .../build/qemu/arch/seam_defaults/arch_idle_wait_default.cc:10
#1  0x000051b6 in kickos::(anonymous namespace)::idle_entry () at .../kernel/init/kmain.cc:160
(gdb) p kickos::sched::current()->name
$1 = "idle", '\000' <repeats 11 times>
(gdb) set $p = (kickos::Thread*)0x20002660
(gdb) p/x $p->ctx
$2 = {sp = 0x20001650, npriv = 0x0, resting_npriv = 0x1, stack_lo = 0x2001e180, stack_hi = 0x20020000, kernel_sp = 0x20001710, exc_return = 0xfffffffd}
(gdb) info symbol *(unsigned*)($p->ctx.sp + 56)
kickos::(anonymous namespace)::sleep_park(unsigned long long, bool) + 84 in section .text
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) p $k->sleepqs.cells[0]->deadline_ns
$3 = 429888440
(gdb) p $k->timer_armed_ns
$4 = {429888440}
```

`ctx.sp` points into the kernel stack. Eight software-saved words sit there, then the hardware
frame, whose seventh word (offset 32 + 24 = 56) is the saved `pc`: `sleep_park + 84`, its closing
brace, where the lock was released. `exc_return` is the `EXC_RETURN` code PendSV will return
through. `npriv = 0`: `ping` will resume privileged, mid-syscall, while its `resting_npriv = 1`
remembers that it goes back to user mode afterwards. The sleep queue head is `ping`, and the timer
is armed for exactly its deadline.

**Why, and the contract:** [Book: the time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md)
(the emulator clock), [Reference: invariants](../reference/invariants.md) (`exit-parks-for-deferred-switch`
for the other place that waits in `arch_idle_wait`)

### Stop 10: the timer fires
**Where:** `arch/arm/common/arch_arm_common.cc`, `SysTick_Handler`; `kernel/time/time.cc`,
`kickos_isr_timer` and `ktime_on_timer`

SysTick reaches zero and raises its exception, number 15. The hardware stacks idle's registers and
enters `SysTick_Handler`, which calls `kickos_isr_timer`, which calls `ktime_on_timer`:

```
    void ktime_on_timer()
    {
        IrqLock lock;
        // BEFORE anything reads the queue: the comparator has fired, so what the kernel
        // recorded for it no longer describes the hardware.
        ktime_disarm(lock);
        uint64_t now = ktime_now();
        // ...
        sched::tick_rr(now, lock);

        while (true)
        {
            Thread* const t = kernel().sleepq(kickos_kernel_core());
            if (t == nullptr or t->deadline_ns > now)
            {
                break;
            }
            sleepq_remove(t);
            // sleepq_remove leaves the wait edge: a timed wait is on the sleepq AND a wait
            // queue at once, and the unwind reads the edge to find the second.
            thread_abort_park(t, -KOS_ETIMEDOUT, lock);
        }

        ktime_rearm(sched::current(), lock);
    }
```

First it forgets what was armed (`ktime_disarm` sets `timer_armed_ns` to `UINT64_MAX` and stops the
counter), because the record no longer matches the hardware. Then it reads the clock once,
charges the running thread's time slice (`tick_rr`, which matters only for round-robin threads),
and takes every thread whose deadline has passed off the front of the sorted queue. If the timer
fired early, because a long sleep was clamped in Stop 7, the loop finds nobody due and the final
`ktime_rearm` arms the remainder. At the end it arms the timer for whatever is next, or leaves it
off.

**Try it:** `break SysTick_Handler`, `continue`, `bt 3`, `p/x $xpsr`:

```
Breakpoint 7, SysTick_Handler () at .../arch/arm/common/arch_arm_common.cc:262
#0  SysTick_Handler () at .../arch/arm/common/arch_arm_common.cc:262
#1  <signal handler called>
#2  0x0000d362 in arch_idle_wait () at .../build/qemu/arch/seam_defaults/arch_idle_wait_default.cc:11
$12 = 0x100000f
```

gdb shows the exception entry as `<signal handler called>`; below it is the code that was
interrupted, idle's loop. `xPSR` ends in 0xf, exception 15.

**Why, and the contract:** [Book: interrupts and traps](../book/interrupts-and-traps.md),
[Reference: invariants](../reference/invariants.md) (`timer-arm-dedup-needs-disarm`,
`timer-disarm-clears-pend`, `rr-quantum-is-wall-clock`)

### Stop 11: the wake
**Where:** `kernel/thread/park.cc`, `thread_abort_park`, `thread_abort_park_no_resched`;
`kernel/sched/sched.cc`, `wake_no_resched`, `resched_after_wake`

`thread_abort_park` is the general way to end a park from outside, used by timeouts and by kills.
It looks at the wait edge to undo the park: for a wait on a semaphore's word it unlinks the
thread from the word's queue and frees the word's record if that queue is now empty, and for a
mutex or an endpoint it also reverts priority donations. For `WAIT_SLEEP`
there is nothing to unlink, because the thread was on no wait queue. Then, for every kind:

```
        t->clear_wait_edge();
        t->wait_result = result;
        return sched::wake_no_resched(t, held);
```

`wait_result` is the per-thread slot through which a waker hands a blocked thread its outcome. The
timer always writes `-KOS_ETIMEDOUT`, because for a semaphore wait with a timeout that *is* the
outcome. The waker writes it; the parked thread never does.

`wake_no_resched` cancels any pending deadline (`ping`'s is already off the queue), refuses
anything not `BLOCKED`, marks the thread `READY` and links it on its ready list. Then
`thread_abort_park` calls `resched_after_wake`, which runs the same `pick_and_seat` as Stop 6. `ping` at priority 10
beats idle, so `switch_to(ping)` runs: `switch_book` makes `ping` current, and `arch_switch` sets
PendSV pending again. We are inside the SysTick handler, so PendSV cannot run yet; it runs right
after `SysTick_Handler` returns, the hardware chaining one exception into the next without
returning to idle in between.

**Try it:** `break kickos::thread_abort_park`, `continue`, then `break arch_switch`, `continue`,
`finish`, and read the interrupt control register `ICSR` at 0xE000ED04:

```
Breakpoint 8, kickos::thread_abort_park (t=0x20002660 <kickos::(anonymous namespace)::system_pools+6944>, result=-110, held=...) at .../kernel/thread/park.cc:132
(gdb) p t->name
$13 = "ping", '\000' <repeats 11 times>
(gdb) p t->wait_kind
$14 = kickos::WAIT_SLEEP
(gdb) continue
Breakpoint 9, std::__atomic_base<arch_context*>::store (this=0x20000030 <g_arch_next>, __p=0x20002660 <...>, ...)
(gdb) p/x $r0
$15 = 0x20000640
(gdb) p ((kickos::Thread*)$r1)->name
$16 = "ping", '\000' <repeats 11 times>
(gdb) finish
arch_switch (from=0x20000640 <kickos::detail::g_instance+344>, to=0x20002660 <...>) at .../arch/arm/common/arch_arm_common.cc:98
(gdb) finish
kickos::ktime_on_timer () at .../kernel/time/time.cc:398
(gdb) p/x *(unsigned*)0xE000ED04
$17 = 0x1000e80f
```

`result=-110` is `-KOS_ETIMEDOUT`. `from` is idle's context (0x20000640, idle's control block sits
in the kernel instance itself), `to` is `ping`'s. In
`ICSR`, bit 28 (0x10000000) is PendSV pending, and the low byte 0x0f says the active exception is
still SysTick.

**Why, and the contract:** [Book: the blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
("The wake protocol: pop, transfer, wake", "The status channel: `wait_result`")

### Stop 12: ping resumes, and returns 0
**Where:** `kernel/time/time.cc`, the end of `sleep_park`; `kernel/syscall/syscall.cc`,
the `KOS_SYS_SLEEP` case; `arch/arm/armv7m/switch.S`, the end of `svc_trampoline`

PendSV restores `ping`'s registers from its kernel stack, sets `CONTROL.nPRIV` to the saved 0, and
returns from the exception to the saved `pc`: the closing brace of `sleep_park`, right
where the lock was released. From `ping`'s point of view, `sched::reschedule` returned and the
lock opened; 0.4 s simply passed. `sleep_park` returns, `ktime_sleep` returns 0, and the
`KOS_SYS_SLEEP` case hands that 0 back. The trampoline then takes the way back described in
[the first syscall](02-first-syscall.md), Stop 11: back to the user stack, privilege dropped to
`resting_npriv`, `bx lr` into the user stub `arch_syscall`, which returns to `sleep_limit` and
`kos_sleep_for`.

Notice what happened to `-KOS_ETIMEDOUT`. It sits in `ping`'s `wait_result`, and nobody reads it:
for a sleep, reaching the deadline is the success. A semaphore wait or a send reads
`wait_result` after waking, and those calls first wait for `wq_confirm_resume` to see
`switch_count` move past the epoch `park_queueless` returned, because on a deferred-switch core
"the line after the park" can run before the switch happened. A sleep reads nothing, so it needs
no such check.

**Try it:** the instruction after `bl syscall_dispatch` in `svc_trampoline` has no symbol;
`disassemble svc_trampoline` shows it, at 0xe0e in this build. gdb also needs to be told the
language there, since the trampoline is assembly. After the SysTick stop:

```
(gdb) delete
(gdb) set language c++
(gdb) break *0xe0e
(gdb) continue
Breakpoint 4, svc_trampoline () at .../arch/arm/armv7m/switch.S:691
(gdb) p kickos::sched::current()->name
$17 = "ping", '\000' <repeats 11 times>
(gdb) p kickos::sched::current()->wait_result
$18 = -110
(gdb) p kickos::sched::current()->switch_count
$19 = {v_ = {... _M_i = 2} ...}
(gdb) info registers r0 r1
r0             0x0                 0
r1             0x0                 0
(gdb) p $control
$20 = 2
```

`ping` is back, inside its trampoline, with the result 0 in `r0:r1` and the unread `-110` in
`wait_result`. `switch_count` is 2: switched in once at its start, once now. `CONTROL` is 2,
privileged. `break *0xe32` (the final `bx lr`) and `continue` shows `$control` back at 3 before
the return to `arch_syscall`. The next line `ping` runs prints `ping 1`.

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md)
("Blocking syscalls are ordinary context switches"), [Book: the blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
("The fix: confirm the resume, then read outside the lock"),
[Reference: invariants](../reference/invariants.md) (`syscall-restores-resting-priv`)

## On the silicon twin

`f411disco` runs every line of this chapter except one chip seam: `arch_clock_now` reads TIM2
through the anchor instead of the dual timer (Stop 3). Idle runs the same default `wfi` (Stop 9),
and the one-shot timer is the same SysTick code in `arch/arm/common/arch_arm_common.cc`, at the
F411's own `SystemCoreClock`.

## Where to next

- [4. Mutex](04-mutex.md): a park on a wait queue, a hand-over by the waker, and priority
  inheritance.
- [5. Call and reply](05-call-and-reply.md): a park that carries a message, and a timed wait that
  sits on a wait queue and the sleep queue together.
- [7. An interrupt to a driver](07-interrupt-to-a-driver.md): another interrupt that wakes a thread.
- [11. Multicore](11-multicore.md): one sleep queue, one `timer_armed_ns` cell per core.
- [14. RISC-V](14-riscv.md): the same stops with the CLINT `mtime` clock and compare register.

## Exercises

1. Change `BEAT_NS` in `hello` to 1 s. SysTick holds at most 0x00FFFFFF cycles (`SYST_RVR_MAX`,
   `arch/arm/common/regs.h`), about 0.67 s at 25 MHz. Predict how many times `SysTick_Handler`
   runs per sleep and what `ktime_on_timer`'s loop does on the first one. Check with a counting
   breakpoint (`break SysTick_Handler` with `commands`, `silent`, a `printf`, `continue`).
2. `kos::sleep_for(std::chrono::nanoseconds{0})` never parks. Predict which function it reaches
   instead of `sleep_park`, and confirm with `break kickos::sched::yield`.
3. In Stop 7 the switch to idle did not re-arm the timer. Predict what would happen to `ping` if
   `ktime_rearm` re-armed SysTick on every switch and the system were busy switching between two
   threads every 100 ms. The comment above `arch_timer_arm` and the invariant
   `timer-arm-dedup-needs-disarm` give the answer.
