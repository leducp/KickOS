<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The kernel model: who runs, when, and what they wait for

> This chapter opens Part 2. It teaches, at the overview level, the objects at the
> heart of any kernel (threads, the scheduler, synchronization objects and time) and
> the choices KickOS makes for each: priority-based preemptive scheduling, tickless
> time, one way to block and wake, and one kernel lock. It then says which
> sub-chapter answers which question. The exact contracts live in
> `../reference/architecture.md` ("Scheduler", "Synchronization surface") and
> `../reference/invariants.md`.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.2 (processes and threads,
> interprocess communication, scheduling), ch.5 (the section on clocks) and ch.8
> (multiprocessor synchronization and scheduling).*

## Why a kernel needs a model of execution

Chapter 0.2 said that a kernel shares the CPU among programs. Sharing is the whole
problem of this part. A core runs one instruction stream at a time, and a system has
more work than cores: a control loop, a UART driver, a logger, the app's `main`. The
kernel must answer four questions, over and over:

1. **What is a unit of work?** Something the kernel can stop and later resume.
2. **Which unit runs now?** And when does that choice change?
3. **How does a unit wait?** For a message, a lock, a device or a moment in time,
   without burning the CPU it does not need.
4. **How is the kernel's own state kept consistent** while threads, interrupts and
   other cores all reach into it?

Every kernel answers them, and the answers form its *kernel model*. The sections below
take the questions in order. Each one gives the general idea, the main options, and
what KickOS chose. The sub-chapters then go deep on the parts that are subtle.

## The core objects

**A thread** is the unit of execution: a saved set of registers, a stack, and a state.
The kernel keeps them in a **thread control block** (TCB). To run a thread is to load
its registers into the core; to stop it is to save them back. **A process**, in
Tanenbaum's sense, is the unit of *ownership*: an address space and the resources its
threads share. Classic Unix merges the two (one process, one thread); most modern
kernels separate them.

Around the threads sit three more pieces:

- **The scheduler** decides which ready thread runs on each core.
- **Synchronization objects** let a thread wait for another thread, or for an
  interrupt: a semaphore, a mutex, a message endpoint.
- **Time** lets a thread wait for a moment, and lets the scheduler bound how long one
  thread may run.

**What KickOS keeps.** The TCB is `Thread` in `kernel/include/kickos/thread.h`. It
holds the saved context, the stack bounds, the priority, the scheduling policy, the
protection regions, and the thread's own capability table. A **task** (`Task`,
`kernel/include/kickos/task.h`) is KickOS's process: a group of threads that share one
memory domain and live and die together. Chapter 7 covers the domain; this part only
needs the thread.

A thread moves through a small set of states (`ThreadState`):

```
            wake (an object, a timer, a cancel)
     +-------------------------------------------+
     v                                           |
  READY ---- picked ----> RUNNING ---- park ---> BLOCKED
     ^                       |
     +---- preempted --------+----- exit ----> EXITED
```

`BLOCKED` covers every kind of wait. A field on the thread, its wait kind, says which
one: a semaphore, a mutex, an endpoint, a sleep, a join, and a few more.

Two kinds of thread are special. The **root thread** is the first one the kernel
creates at boot. It is unprivileged from its first instruction, runs the image's global
constructors, then calls the system's init, which starts the tasks the system is made
of and grants each its authority. In the f411 example, one of those tasks runs the
app's `main`, whose thread calls `printf("hello\n")`, and that `main`'s return ends the
system. Each core also has an **idle thread** at the lowest priority, which runs only
when nothing else can, and puts the core to sleep until an interrupt arrives.

The kernel's objects live in fixed pools sized at build time. Userspace never holds a
pointer to one. It names a semaphore or a mutex through a **handle** in its capability
table, and the handle carries a generation, so a handle to an object that was freed and
reused stops resolving instead of reaching the new occupant. Part 8 teaches handles;
this part assumes them.

## Choosing who runs: priorities, FIFO and round-robin

**Why.** The scheduler's goal depends on the system. A desktop wants fairness and
responsiveness: every program makes progress, and the one the user is typing into feels
fast. A real-time system wants **predictability**: the control loop must run within a
bound every time, whatever else is going on. Tanenbaum ch.2 lists these goals side by
side, and they lead to different designs.

**How one could.**

- **Cooperative or preemptive.** A cooperative scheduler switches only when a thread
  gives up the CPU. A preemptive one can take the CPU away, for example when a more
  urgent thread becomes ready. Real-time needs preemption: an urgent thread cannot wait
  for a busy one to be polite.
- **Dynamic or fixed priorities.** A time-sharing scheduler adjusts priorities as threads
  run, to favour interactive work. A real-time scheduler keeps the priority the designer
  assigned, because the designer's analysis depends on it.
- **Within one priority level.** Threads of equal priority can run first-come,
  first-served until each blocks (**FIFO**), or take turns, each for a fixed slice of
  time, its **quantum** (**round-robin**, RR).
- **Deadline-driven policies** such as earliest-deadline-first choose by deadline rather
  than by a fixed priority.

**What KickOS chose.** Fixed-priority, preemptive scheduling. A larger number is more
urgent, from `KICKOS_PRIO_MIN` to `KICKOS_PRIO_MAX`, and the idle thread sits below
them all. The highest-priority ready thread always runs. Within one level, each thread
picks its own policy: FIFO, or RR with its own quantum. A FIFO thread runs until it
blocks, yields or is preempted by a more urgent one. An RR thread also gives way to an
equal-priority peer when its quantum ends. This is the classic real-time model, and its
behaviour is easy to state, which is what predictability needs.

A task also carries a **priority ceiling**, granted when the task is created. Its
threads may set their own priorities freely below it, but never above, so an
unprivileged thread cannot take the top of the run queue.

**How it is done.** The policy lives behind a small set of `policy_*` functions
declared in `kernel/include/kickos/sched.h`, implemented by
`kernel/sched/policy_fifo_rr.cc`: one FIFO list per priority and a bitmap of the
non-empty levels, so the highest ready level is found with one count-leading-zeros. The
rest of the scheduler, in `kernel/sched/sched.cc`, owns the mechanism: thread states and
the context switch. A different policy would supply the same functions.

Every reason to switch ends in one function, `pick_and_seat`: a yield, a block, a wake
from a thread, a wake from an interrupt, an expired sleep, an expired quantum. None of
them is special. In particular, **the timer is not the scheduler's heartbeat**: when a
device interrupt readies a thread, the switch happens on the way out of that interrupt,
without waiting for any tick.

## Time without a tick

**Why.** Threads sleep, waits time out, and round-robin needs to know when a quantum
ends. All three need a clock and a way to be woken at a chosen moment.

**How one could.** The traditional answer is a **periodic tick**: a timer interrupts at
a fixed rate, say 1 kHz, and on each tick the kernel counts down every sleep and every
quantum. It is simple. It also interrupts when nothing is due, which wastes power, and
it rounds every delay to a whole tick. The other answer is **tickless** time: read a
free-running counter for "now", and program a one-shot timer for the earliest thing
actually due.

**What KickOS chose.** Tickless, by default. The kernel asks the hardware for
two things only, both declared in `arch/include/kickos/arch/arch.h`:
`arch_clock_now`, a monotonic time in nanoseconds, and `arch_timer_arm`, a one-shot
interrupt at an absolute deadline. Every deadline is stored as an absolute time on a
sorted list, and the one-shot is armed for the earliest of them, or for the end of the
running thread's quantum if that comes sooner. A system with nothing timed arms nothing
and takes no timer interrupt at all.

**The f411 example.** Chapter 0.3 met TIM2 on interrupt line 28: it is this chip's
`arch_clock_now`, a 32-bit counter whose wraps the TIM2 handler adds into a 64-bit
count. The one-shot is a different timer, the core's own SysTick. So on the f411, "what
time is it" and "wake me at T" are two peripherals, and the kernel above them only sees
the two functions.

A tickless kernel is exactly as trustworthy as its clock: a "now" that jumps forward
once makes every armed deadline look already past. Chapter 2.1 explains why, and what a
porter checks before trusting a counter.

## Blocking and waking

**Why.** A thread that waits for a semaphore, a lock or a message must not spin: on one
core, spinning would starve the very thread it waits for. It must **block**: leave the
ready set, and come back when the event happens. The classic trap is the **lost
wakeup**. A thread checks the condition, decides to wait, and before it is recorded as
waiting, the event arrives and its signal lands on nobody. The check and the park must
be one indivisible step with respect to whoever wakes.

**How one could.** Each synchronization object can implement its own park and wake,
with its own chance to get the lost wakeup wrong. Or all of them can share one
mechanism, and add only what is really their own.

**What KickOS chose.** One mechanism, and few objects. Every blocking call parks the
thread the same way, and every wake goes through the same protocol. The wait queue and
the ready queue even share one link in the TCB: a thread is either ready or waiting,
never both. On top of that substrate the kernel offers only two synchronization objects
(`kernel/include/kickos/sync.h`):

- **The counting semaphore**, the general wait/wake object. Its `post` is safe from an
  interrupt handler, which is how an interrupt readies a thread.
- **The priority-inheritance mutex**, the one object justified beyond the semaphore.
  It exists because of **priority inversion**: a low-priority thread holds a lock that
  a high-priority thread wants, and a medium-priority thread, unrelated to the lock,
  keeps the low one from running and releasing it. The cure, lending the holder the
  waiter's priority, is a scheduler action that userspace cannot perform, so it must be
  in the kernel. A lock without that is just a binary semaphore.

Condition variables, read-write locks and barriers are userspace libraries over those
two. Endpoints (Chapter 8.3) and the notifications that carry interrupts to drivers
(Chapter 3) park on the same substrate.

**The f411 example.** On their way out through the kernel console (Chapter 0.2), the
bytes of `printf("hello\n")` go into its transmit ring, which the USART2 transmit
interrupt drains into the data register at `0x40004404`. If the ring has no room, the
writing thread parks (`console_room_wait`, `kernel/thread/park.cc`). When the drain
frees enough room, it calls `console_tx_room_freed`, which wakes the writer from
interrupt context. The thread resumes inside its system call, as if the call had simply
taken longer.

## One lock, and short critical sections

**Why.** The scheduler's lists, the wait queues, the timer list and the capability
tables are shared. A system call edits them, an interrupt handler edits them, and on a
multicore part another core edits them. Each edit must look atomic to everyone else:
an interrupt must never see a thread taken off the ready list but not yet on a wait
queue.

**How one could.**

- **Mask interrupts.** On one core, code that runs with interrupts masked cannot be
  interrupted, so a critical section is just a masked span. This does nothing against
  another core.
- **One big kernel lock** (BKL). Every core takes the same lock to enter kernel state.
  Simple to reason about; it serializes the kernel's work across cores.
- **Fine-grained locks**, one per structure. More parallel, with a lock order to keep
  and deadlocks to avoid.
- **Lock-free structures**, which use atomic instructions instead of locks. Fast and
  hard to get right.

**What KickOS chose.** One critical-section type, `IrqLock`
(`kernel/include/kickos/irqlock.h`). Constructing it masks this core's interrupts, and
above one core also takes the single kernel lock; destroying it releases them in the
reverse order. It nests: an inner `IrqLock` records the state it found and restores
exactly that. On the f411's Cortex-M4 it raises **BASEPRI**, so interrupts at the
kernel's level or less urgent wait, and their pending bits hold them until the mask
lifts.

The type system enforces it. A kernel function that needs the exclusion takes a `Held`
token (`kernel/include/kickos/held.h`), and only an `IrqLock`, or the IPC fast path
entered under its trap's own mask, can make one. A call that forgets the lock does not
compile.

Two consequences shape the rest of the kernel:

- **Critical sections stay short.** Every masked instruction adds to the worst-case
  latency of every interrupt the mask holds off. This is one reason the kernel keeps
  its paths small and its work bounded.
- **A switch may wait for the lock.** On some architectures the context switch is
  *deferred*: the kernel requests it, and it happens when the lock opens. On Cortex-M
  that is the PendSV exception, at the lowest priority. A thread that has just parked is
  therefore still running for a few instructions, which is a hazard Chapter 2.2 closes.

**The f411 example.** The clock read itself is a critical section. TIM2's counter and
the 64-bit wrap count are two values; the read takes them with interrupts masked, so the
TIM2 handler cannot add a wrap between the two reads (Chapter 0.3).

Not everything shared goes under the lock. A single word that one context writes and
another reads, such as a console ring index the producer and the drain each advance,
can be a C++ atomic instead. Chapter 2.4 explains what that atomic buys. On a multicore
part, KickOS keeps one kernel lock but gives each core its own ready queue, which is
Chapter 2.5.

## How the pieces fit: one sleep

A thread on the f411 calls `kos_sleep_ns(400000000)`. The pieces above meet like this:

1. The trap enters the kernel, which takes an `IrqLock`.
2. The kernel reads `arch_clock_now` (TIM2), adds 400 ms, and stores that absolute
   deadline on the timer list.
3. The thread becomes `BLOCKED`, waiting on a sleep and on no wait queue.
4. `pick_and_seat` chooses the next ready thread, or the idle thread.
5. The kernel re-arms the one-shot, SysTick, for the earliest deadline.
6. The lock opens, PendSV runs, and the core switches away.
7. When SysTick fires, its handler readies the thread, and `pick_and_seat` runs again.
   If the sleeper is the most urgent ready thread, it resumes inside its system call
   and returns to its caller.

The Journey's [sleep](../journey/03-sleep.md) chapter follows each step through the
code, with gdb.

## Reading map

- **2.1, *[The time base: a monotonic clock you can
  trust](the-time-base-a-monotonic-clock-you-can-trust.md)*.** What `now` and a
  one-shot must guarantee, the options for a time base, and the checklist a porter runs
  before trusting a hardware counter.
- **2.2, *[The blocking substrate: one wait/wake
  primitive](the-blocking-substrate-one-wait-wake-primitive.md)*.** How a thread parks
  and is woken, how a waker hands it a result, how a timed wait fits the same shape, and
  the stale-read hazard of a deferred switch.
- **2.3, *[Priority inheritance: lending a thread its blocker's
  urgency](priority-inheritance-lending-urgency.md)*.** Priority inversion, plain
  inheritance against the priority ceiling, the boost along a chain of owners, and the
  drop on unlock.
- **2.4, *[An atomic buys definedness, not
  atomicity](an-atomic-buys-definedness-not-atomicity.md)*.** What `volatile` and an
  atomic each promise for a word two contexts share, and where an atomic stops being
  enough and a lock begins.
- **2.5, *[One kernel lock, many ready queues](one-lock-many-ready-queues.md)*.** One
  ready queue or one per core, why that choice is separate from how the kernel is
  locked, and why KickOS pairs per-core queues with one lock.

## In the code, and the contract

The Journey walks the code these chapters explain:

- [3. Sleep](../journey/03-sleep.md): the clock, the deadline, the park, the one-shot,
  idle and the wake, on one core.
- [4. A mutex](../journey/04-mutex.md): the uncontended and contended lock, priority
  inheritance and its chain, and the semaphore beside it.
- [11. Multicore](../journey/11-multicore.md): per-core ready queues, the kernel lock,
  and a wake handed to another core.

The Reference holds the exact rules: `../reference/architecture.md` ("Scheduler",
"Synchronization surface"), and in `../reference/invariants.md` the sections "Critical
section & interrupt-priority bands", "Deferred switch & blocking model", "Tickless timer
& clock" and "Scheduler & instance state".

## Where to go next

- Start the part with the clock: Chapter 2.1, *[The time
  base](the-time-base-a-monotonic-clock-you-can-trust.md)*.
- What a context switch saves, and why it is deferred on some cores: Chapter 3.5,
  *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)*.
- How an interrupt reaches a userspace driver: Chapter 3, the interrupt model.
- How a thread names a semaphore or a mutex: Chapter 8.1, *[The handle and the resolve
  chokepoint](handles-and-the-resolve-chokepoint.md)*.
- The code from the start: [Journey 3, Sleep](../journey/03-sleep.md).
