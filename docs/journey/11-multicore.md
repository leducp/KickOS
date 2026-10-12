<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 11. Multicore: one kernel on four cores, then one kernel per core

The reference walk ran on one core. This chapter runs the same kernel on four cores of QEMU's
`virt` machine (`qemu-arm64-smp`, four Cortex-A53) and follows only what changes. By the end you
will have watched three parked cores woken by firmware, each one find its own per-core block and
idle thread, and all four enter their own scheduler. You will then follow one wake across cores:
the waker decides the woken thread belongs on an idle peer, hands it over on a ring, and rings
that peer's doorbell, an inter-processor interrupt (IPI). You will see the one kernel lock every
core takes, what it guards and what it leaves to per-core ownership, the TLB maintenance a page
unmap owes the other cores, and the idle loop each core falls back to.

The second half is AMP (asymmetric multiprocessing): several independent kernels, each driving
its own core, that share a machine. They share no kernel state at all. They share one memory
window with rings in it, one doorbell, and one console.

The design is in the Book chapter [One kernel lock, many ready queues](../book/one-lock-many-ready-queues.md),
which explains why KickOS keeps one big kernel lock (BKL) and still gives each core its own ready
queue. The exact rules are in the Reference: [Scheduler](../reference/architecture.md#scheduler-the-core-constraint)
and the [scheduler invariants](../reference/invariants.md#scheduler--instance-state). The full
design record, with the reasons behind each choice, is [docs/design-multicore.md](../design-multicore.md).
This chapter assumes you have read [the first syscall](02-first-syscall.md),
[sleep](03-sleep.md) and [the MMU](10-the-mmu.md), whose machine this is.

Words used below:

- **SMP** (symmetric multiprocessing): one kernel image, one set of kernel objects, run by
  several cores at once. KickOS calls each such core a *kernel core*.
- **BKL**: the big kernel lock, one lock that any core holds while it changes shared kernel
  state.
- **Doorbell**: KickOS's name for its IPI. One interrupt line per core, which another core can
  raise. On this machine it is the GIC's software-generated interrupt 0 (an SGI).
- **Rendezvous**: a doorbell that waits for an answer. The sender bumps a request counter,
  raises the doorbell, and spins until the target writes the same number into its answer cell.

## Running it under gdb

The image is `hello`, the two-thread ping-pong from the reference walk, built for four cores.
`ctest --test-dir build/qemu-arm64-smp -N -V -R qemu_arm64_hello` prints the
emulator line. Add the gdb flags and attach:

```
qemu-system-aarch64 -M virt -cpu cortex-a53 -nic none -smp 4 -nographic -semihosting \
    -kernel build/qemu-arm64-smp/user/apps/common/hello/hello \
    -gdb tcp::1234 -S &
gdb-multiarch build/qemu-arm64-smp/user/apps/common/hello/hello \
    -ex 'target remote :1234'
```

QEMU shows each emulated CPU to gdb as a *thread*. These are CPUs, not KickOS threads: `info
threads` lists one row per core, `thread 3` switches gdb to CPU#2, and `bt` or `info registers`
then speak about that core. Three habits:

- gdb runs in **all-stop** mode: when any core hits a breakpoint, every core stops. The
  `[running]`/`[halted ]` tag in `info threads` is QEMU's view of the vCPU, where `halted` means
  it is sleeping in `wfi`.
- `stepi` or `next` lets *all* cores run while one steps, so another core may hit a breakpoint
  first and gdb switches to it. `set scheduler-locking step` keeps the others frozen while you
  step.
- `kernel()` is an inline function gdb cannot call. The one kernel instance is the global
  `kickos::detail::g_instance.per_instance[0]`; `set $k = &kickos::detail::g_instance.per_instance[0]`
  once and print through `$k`.

The gdb output below is trimmed: paths are shortened to `...`, line numbers dropped, and
addresses are from one run. The serial console prints three extra lines on this image, which
the stops below explain:

```
# smp: 4 core(s) online
# doorbell: 4 core(s) answered, rounds 0x40, unmoved 0x3
...
# smp sched: 4 core(s) in the scheduler
```

## Part 1: SMP

### Stop 1: four cores at reset, three held by firmware
**Where:** `arch/arm64/chip/virt_arm64/startup.S` (`_start`); `arch/arm64/chip/virt_arm64/chip_virt_arm64.cc`, `psci_hvc`

On the reference walk, reset had one core to start. Here QEMU creates four, but only CPU#0 runs
`_start`. The other three are held inside QEMU's own firmware, which implements PSCI (the Arm
Power State Coordination Interface): a standard call that asks the firmware to power a core on
at a given address. Nothing in the KickOS image runs on cores 1 to 3 until core 0 asks.

So the boot path of [Power-on](01-power-on.md) runs unchanged on core 0: stacks, `.bss`, the
constructors, `arch_init`, `kmain`. What is new is a short list of things that must be in place
*before* another core can run kernel code, and the order of the stops below follows that list.

**Try it:** with the emulator stopped at reset, `info threads`:

```
  Id   Target Id                    Frame
* 1    Thread 1.1 (CPU#0 [running]) _start () at .../startup.S
  2    Thread 1.2 (CPU#1 [halted ]) _start () at .../startup.S
  3    Thread 1.3 (CPU#2 [halted ]) _start () at .../startup.S
  4    Thread 1.4 (CPU#3 [halted ]) _start () at .../startup.S
```

All four report `_start` only because they share a reset address; three are halted and will not
fetch it.

**Why, and the contract:** [Reference: porting, "Which core am I"](../reference/porting.md),
[design-multicore.md N5 (bring-up)](../design-multicore.md)

### Stop 2: what the boot core publishes before any release
**Where:** `arch/arm64/armv8a/arch_armv8a.cc`, `armv8a_boot_publish`, `kickos_armv8a_boot_regs`, `kickos_armv8a_boot_sp`, `kickos_armv8a_percpu`

A released core arrives with its MMU off and nothing set up. It has to come up under exactly the
translation the boot core runs with, so the boot core writes its own system registers (MAIR, TCR,
TTBR0, TTBR1, SCTLR, CPACR) into a record, `kickos_armv8a_boot_regs`, and fills
`kickos_armv8a_boot_sp` with one stack top per core. This runs as a C++ constructor, after `.bss`
is zeroed and before `arch_init`, while TTBR0 still holds the boot page tables rather than some
process's.

Two details matter on real silicon. With its MMU off, a core's loads are non-cacheable, so the
record is cleaned out of the boot core's data cache (`arch_dcache_flush`) or the secondary would
read stale zeroes from RAM. And the boot core seats its own TPIDR_EL1 here: that register holds
the address of this core's **per-CPU block** (`struct armv8a_percpu_block`), the one place an
exception vector can find per-core state without a thread control block. Each core gets a whole
64-byte cache line, so no two cores ever write the same line.

**Try it:** `break armv8a_boot_publish`, `continue`. Only CPU#0 has moved; the others are still
halted at `_start`. Then `finish` (it returns into `kickos_crt_ctors`, the loop over the
constructors) and read what it wrote:

```
(gdb) p/x kickos_armv8a_boot_regs
$1 = {mair = 0x4400ff, tcr = 0x12b5193519, ttbr0 = 0x40001000, ttbr1 = 0x40002000, sctlr = 0xc5183d, cpacr = 0x300000}
(gdb) p/x kickos_armv8a_boot_sp
$2 = {0xffffff804033ab30, 0xffffff804034ab30, 0xffffff804035ab30, 0xffffff804036ab30}
```

The two table roots are physical addresses, because the secondary will load them with its MMU
off. The four stack tops are 64 KiB apart, one `KICKOS_ARMV8A_CORE_STACK` each.

**Why, and the contract:** [Reference: porting, "Which core am I"](../reference/porting.md)

### Stop 3: the release, and the wait for arrival
**Where:** `arch/arm64/chip/virt_arm64/chip_virt_arm64.cc`, `arch_init`, `release_secondaries`, `kickos_armv8a_core_online`

`arch_init` brings up the interrupt controller (the GIC distributor once for the machine, then
this core's own interface) and then calls `release_secondaries`. That function asks PSCI to start
each other core at `kickos_armv8a_secondary_entry`:

```cpp
        uint64_t const cluster = mpidr & MPIDR_AFFINITY & ~uint64_t(0xFF);
        for (uint32_t index = 1; index < KICKOS_NUM_CORES; index++)
        {
            // context_id in x3, which PSCI hands the entry point in x0. Zero: a secondary
            // reads its identity out of MPIDR_EL1.
            long const rc = psci_hvc(PSCI_FN64_CPU_ON, cluster | index, entry, 0);
            if (rc != PSCI_SUCCESS)
            {
                kpanic_enter();
                kickos::kprintf(BAD_CPU_ON, index >> 4, index & 0xFu, rc);
                kfault_terminate();
            }
        }
```

`psci_hvc` is a hypervisor call (`hvc #0`) with the function number in x0, the standard calling
convention for firmware services. The `entry` address must be a *physical* address in the
identity-mapped part of the image, because the new core fetches it with translation off; the
function refuses a high-half entry before it asks.

PSCI answering "success" only means the core was started. The function then waits, with a time
bound, until every core has set its byte in `kickos_armv8a_core_online`, which a core does only
once it is running kernel code on its own stack. Then it prints `# smp: 4 core(s) online` and runs
`kickos_doorbell_boot_check` (Stop 11), which proves the doorbell works before anything depends on
it. A core that never arrives is fatal: `KICKOS_NUM_CORES` sizes every per-core array, so running
on with a missing core would hand threads to a core that does not exist.

**Try it:** `break kickos_armv8a_secondary_entry` then `continue`. The first released core stops
there, and `info threads` now shows the release in progress:

```
Thread 2 hit Breakpoint 1, kickos_armv8a_secondary_entry () at .../secondary.S
  1    Thread 1.1 (CPU#0 [running]) 0xffffff804001cf40 in (anonymous namespace)::release_secondaries () at .../chip_virt_arm64.cc
* 2    Thread 1.2 (CPU#1 [running]) kickos_armv8a_secondary_entry () at .../secondary.S
  3    Thread 1.3 (CPU#2 [running]) kickos_armv8a_secondary_entry () at .../secondary.S
  4    Thread 1.4 (CPU#3 [running]) kickos_armv8a_secondary_entry () at .../secondary.S
```

Core 0 is in the arrival wait; `p/x $pc` on the secondary reads `0x400001a0`, a low physical
address, while core 0's PC is in the `0xffffff80...` high half.

**Why, and the contract:** [design-multicore.md N5](../design-multicore.md)

### Stop 4: a secondary's first instructions
**Where:** `arch/arm64/armv8a/secondary.S`, `kickos_armv8a_secondary_entry`, `kickos_armv8a_secondary_high`; `arch/arm64/armv8a/arch_armv8a.cc`, `arch_cpu_id`

The secondary masks interrupts, then works out *which core it is* before it touches memory:

```asm
    mrs     x19, mpidr_el1
    and     x19, x19, #0xFF                 /* MPIDR_EL1.Aff0 (arch_armv8a.cc) */
    cmp     x19, #KICKOS_NUM_CORES
    b.hs    .Lsecondary_stop
    cbz     x19, .Lsecondary_stop

    ldr     x9, =__kickos_arm64_va_base

    /* ... */
    ldr     x1, =kickos_armv8a_percpu
    mov     x2, #PERCPU_SIZE
    madd    x1, x19, x2, x1
    msr     tpidr_el1, x1
```

MPIDR_EL1 is a read-only register that names the core; its lowest byte (Aff0) is the core index
on this one-cluster machine. `arch_cpu_id` reads the same field for the rest of the kernel's life,
so "which core am I" is always a register read, never a variable. Index 0 is refused: only the
boot core may be core 0, and a released core that believed it was would take the boot core's
block and stack.

TPIDR_EL1 is set to this core's per-CPU block *before* the MMU comes on, so there is no window
in which an exception could read it unset. The stub then loads the boot core's register record
(through its physical alias, since translation is still off), invalidates its own TLB and
instruction cache, turns the MMU on, and branches through a register to
`kickos_armv8a_secondary_high`, which runs at the kernel's high virtual address, takes its stack
from `kickos_armv8a_boot_sp[index]`, and calls `kickos_armv8a_park_core(index)`.

**Try it:** after the breakpoint of Stop 3, `set scheduler-locking step`, then `stepi 6` and
`p/x $x19`: it now holds the core index, `0x2` when the stopped core is CPU#2. Then `delete`, `break kickos_armv8a_park_core`, `continue`:

```
Thread 3 hit Breakpoint 2, kickos_armv8a_park_core (id=2) at .../arch_armv8a.cc
(gdb) bt
#0  kickos_armv8a_park_core (id=2) at .../arch_armv8a.cc
#1  0xffffff804001bf2c in kickos_armv8a_secondary_high () at .../secondary.S
```

The secondary is now in the high half. `p kickos_armv8a_core_online` shows the boot core's byte
set and this core's still clear: `kickos_armv8a_park_core` sets it next.

**Why, and the contract:** [Reference: porting, "Which core am I"](../reference/porting.md)

### Stop 5: parked, waiting to be allowed in
**Where:** `arch/arm64/armv8a/arch_armv8a.cc`, `kickos_armv8a_park_core`; `arch/arm64/armv8a/klock_armv8a.cc`, `kickos_armv8a_doorbell_park`; `arch/include/kickos/arch/doorbell_protocol.h`, `kickos::doorbell::park`; `kernel/sync/corestart.cc`, `kickos_kernel_core_startable`

`kickos_armv8a_park_core` brings up this core's own GIC interface and timer (each core has its
own), sets its online byte, and parks. The kernel is not ready for it yet: `kmain` has not built
its idle thread. So the core sleeps in a loop that only the doorbell can wake:

```cpp
        while (true)
        {
            // Masked across every test below. A raise taken between a test and the sleep is a
            // wake this core never sleeps on, and the core-start raise is a single edge.
            (void)arch_irq_save();
#if KICKOS_KERNEL_CORES > 1
            if (kickos_kernel_core_startable() != 0)
            {
                // Masked and never restored: the scheduler's first resume stands on a frame
                // carrying its own interrupt state.
                Part::before_start();
                kickos_kernel_core_start();
            }
#endif
            uint32_t const contend = g_contend.load();
            if (contend == CONTEND_PARK)
            {
                // Must unmask and sleep with nothing landing between the two.
                Part::sleep();
                continue;
            }
            // ...
```

Notice the order: interrupts masked, *then* the test, *then* a sleep that unmasks and sleeps as
one step (`wfi` wakes on a pending interrupt even while masked). Testing first and sleeping after
would lose a doorbell that arrived between the two. The rest of the loop (`g_contend`) is only
used by the boot check of Stop 11, which drives parked cores into the lock to test it.

`kickos_armv8a_doorbell_park` also narrows this core's GIC bank to the doorbell alone, so the timer
cannot fire on a core that has no scheduler yet.

**Try it:** `break kickos::corestart_seat` then `continue`. The three secondaries are asleep in
the park's `wfi`:

```
* 1    Thread 1.1 (CPU#0 [running]) kickos::corestart_seat (cores=cores@entry=14) at .../corestart.cc
  2    Thread 1.2 (CPU#1 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
  3    Thread 1.3 (CPU#2 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
  4    Thread 1.4 (CPU#3 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
```

`cores` is `14`, binary `1110`: cores 1, 2 and 3.

**Why, and the contract:** [Book: A masked interrupt is latched, not lost](../book/a-masked-interrupt-is-latched-not-lost.md),
[Reference: porting, "The cross-core doorbell"](../reference/porting.md)

### Stop 6: per-core state, and an idle thread for every core
**Where:** `kernel/include/kickos/instance.h`, `struct Kernel` (`currents`, `idles`, `ready`, `ready_bitmap`, `seated_prio`, `timer_armed_ns`, `sched_out`, `sched_in`); `kernel/init/kmain.cc`, `peer_idle_publish`; `kernel/sched/sched.cc`, `sched::add_idle`

The kernel still has exactly one `Kernel` instance, reached through `kernel()`. What is per core
is a small, named set of fields inside it, each an array indexed by core:

| Field | What it holds |
| --- | --- |
| `currents`, `idles` | the thread this core runs, and its idle thread |
| `ready[core][prio]`, `ready_bitmap[core]` | this core's ready queue: one FIFO list per priority and a bitmap of non-empty lists |
| `seated_prio[core]` | the priority this core last chose to run |
| `timer_armed_ns[core]` | the deadline this core's own timer is set for |
| `sched_out[core]`, `sched_in[core]`, `sched_slot[from][to]` | the handoff rings of Stop 9 |

Everything else in `Kernel` (the thread pool, capability tables, endpoints, the sleep queue of
[Sleep](03-sleep.md), the IRQ table) is shared, and the kernel lock of Stop 8 guards it. The
arch keeps its own per-core state apart, in the per-CPU block of Stop 2, because a vector stub
needs it before any C code runs.

`kmain` creates the boot core's idle thread and root as on the reference walk, adds root, and
only then, with root already runnable, publishes the peers:

```cpp
        void peer_idle_publish()
        {
            uint32_t peers = 0;
            for (KernelCore const core : KernelCores::peers())
            {
                Thread* const tcb = &kernel().idle_tcb_peer[core - 1];
                idle_build(tcb, g_peer_idle_stack[core - 1]);
                sched::add_idle(tcb, core);
                peers |= 1u << core;
            }
            // AFTER every block above is complete: this is the release each peer's wait acquires.
            corestart_seat(peers);
            // Out of the wait a released core sits in, so it looks at its cell.
            arch_ipi_send(peers);
        }
```

Each peer's idle thread lives outside the thread pool (`idle_tcb_peer`), with an affinity of
exactly its own core. `sched::add_idle` links it straight onto the peer's ready queue: the only
time one core writes another core's ready structure, allowed because that core has not started.
`corestart_seat` then sets a per-core `seated` flag with a release store; the peer reads it with
an acquire in `kickos_kernel_core_startable`, which is what guarantees the peer sees the
complete idle thread. The doorbell wakes the peers out of their `wfi` to look.

**Try it:** stopped at `kickos::corestart_seat` as in Stop 5:

```
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) p $k->idles.cells[1]->name
$1 = "idle", '\000' <repeats 11 times>
(gdb) p $k->currents
$2 = {cells = {0x0, 0x0, 0x0, 0x0}}
(gdb) p/x $k->ready_bitmap
$3 = {0x80000001, 0x1, 0x1, 0x1}
```

No core runs anything yet. Core 0's bitmap has bit 31 (root, at the top priority) and bit 0 (its
idle thread); each peer holds only its idle thread.

**Why, and the contract:** [Reference: invariants, `instance-scoped-state-no-arch-crossing` and `ready-bitmap-tracks-lists`](../reference/invariants.md#scheduler--instance-state)

### Stop 7: every core enters its own scheduler
**Where:** `kernel/sync/corestart.cc`, `kickos_kernel_core_start`; `kernel/sched/sched.cc`, `sched::start`; `kernel/init/kmain.cc`, `peer_start_await`

A woken peer finds its `seated` flag set, readies its GIC interface (`Part::before_start`), and
calls `kickos_kernel_core_start`:

```cpp
    extern "C" void kickos_kernel_core_start(void)
    {
        g_row[kickos_kernel_core()].arrived = 1u;
        sched::start();
        while (true)
        {
            arch_idle_wait();
        }
    }
```

`sched::start` is the same function the boot core calls at the end of `kmain`, run on each core
for itself. Under the kernel lock it drains anything already handed to this core (Stop 9), picks
the best thread on its *own* ready queue with `policy_pick_next`, publishes this core's level
(the priority it now runs, read by placement in Stop 9), and jumps into the thread with
`arch_start`. For a peer that first thread is its idle thread.

Meanwhile the boot core spins in `peer_start_await` until every peer has set `arrived`, and
deliberately does so *without* the kernel lock, since each peer needs that lock inside its own
`sched::start`. Then it prints `# smp sched: 4 core(s) in the scheduler` and enters its own
scheduler, where root is waiting.

**Try it:** `break kickos_kernel_core_start` then `continue`, then `bt`:

```
#0  kickos::kickos_kernel_core_start () at .../corestart.cc
#1  0xffffff804001c080 in kickos::doorbell::park<(anonymous namespace)::Part> () at .../doorbell_protocol.h
#2  kickos_armv8a_doorbell_park () at .../klock_armv8a.cc
#3  0xffffff8040019a54 in kickos_armv8a_park_core (id=3) at .../arch_armv8a.cc
#4  0xffffff804001bf2c in kickos_armv8a_secondary_high () at .../secondary.S
```

Then `delete`, `break kickos::sched::start`, `continue`, `info threads`: core 0 is in
`peer_start_await`, and the peers are entering `sched::start` (some still at its first line,
`arch_irq_save`, the `IrqLock` constructor).

**Why, and the contract:** [Reference: architecture, Scheduler](../reference/architecture.md#scheduler-the-core-constraint)

### Stop 8: the kernel lock
**Where:** `kernel/include/kickos/irqlock.h`, `IrqLock`; `kernel/sync/klock.cc`, `klock_enter`, `klock_leave`, `klock_detach`, `klock_attach`, `kickos_switch_unlock`; `arch/arm64/armv8a/klock_armv8a.cc`, `arch_kernel_lock`, `arch_kernel_unlock`; `arch/include/kickos/arch/doorbell_protocol.h`, `await_turn`

On one core, a critical section only has to keep this core's interrupts out (`IrqLock`, which
on the reference walk raises `BASEPRI`: [02, Stop 6](02-first-syscall.md#stop-6-one-door-one-switch)).
Masking stops this core's interrupt handlers, but does
nothing to stop another core. So above one core `IrqLock` does two things, in a fixed order: mask
this core, then take the one kernel lock. The destructor releases the lock, then unmasks.
Reversed, an interrupt handler on a core that holds the lock could spin forever on its own lock.

The lock nests per core: `klock_enter` takes it only when this core's depth goes from 0 to 1,
and `klock_leave` releases it when the depth returns to 0. The arm64 lock itself is a **ticket
lock**: a core draws a number from `g_next_ticket` with an exclusive load/store pair (`ldxr`/`stxr`,
retried until the store succeeds) and waits until `g_now_serving` equals its number:

```cpp
void arch_kernel_lock(void)
{
#if KICKOS_BENCH
    // ...
#else
    uint32_t const ticket = kernel_lock_draw();
#endif
    kickos::doorbell::await_turn([ticket] { return now_serving() == ticket; });
    kickos::klock::owner_take();
}
```

The holder releases by storing `g_now_serving + 1` with `stlr`, a store-release, which pairs with
the waiter's `ldar` load-acquire so everything written under the lock is visible to the next
holder. Tickets are served in order, so the lock is fair. Look at `await_turn`:

```cpp
        while (not turned())
        {
            kickos_doorbell_poll();
            part_spin();
        }
```

A core waiting for the lock has interrupts masked, so it cannot take a doorbell through the
vector. Yet the lock holder might be waiting for *this* core to answer a rendezvous (Stop 11).
The poll in the loop answers it from inside the spin, which is what keeps the two from waiting
on each other forever.

**What the lock protects.** Every shared kernel transaction: resolving a capability, an endpoint
rendezvous, the wait queues, the timer delta list, thread state changes, object pools. A whole
CALL (resolve, copy, mint a reply capability, park the caller, wake the server) is one span under
one lock. Functions that need it take a `Held` token, which only an `IrqLock` can produce, so a
call made without the lock does not compile. The per-core ready queues are edited under the lock
too, but their real rule is ownership: only a queue's own core edits it (Stop 9).

**Across a context switch** the lock is held by the core, not the thread. `switch_to` calls
`klock_detach` (the depth leaves the core, the lock stays held), switches, and the incoming thread
releases it in `kickos_switch_unlock` once the outgoing thread's registers are safely saved. Until
then no other core may pick that thread up.

**Try it:** let the peers start (`break kickos_kernel_core_start`, `continue`), then `delete`,
`break arch_kernel_lock`, `continue`:

```
Thread 2 hit Breakpoint 2, arch_kernel_lock () at .../klock_armv8a.cc
  1    Thread 1.1 (CPU#0 [running]) (anonymous namespace)::counter_now () at .../arch_arm64_a53.cc
* 2    Thread 1.2 (CPU#1 [running]) arch_kernel_lock () at .../klock_armv8a.cc
  3    Thread 1.3 (CPU#2 [running]) (anonymous namespace)::kernel_lock_draw () at .../klock_armv8a.cc
  4    Thread 1.4 (CPU#3 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
(gdb) p '(anonymous namespace)::g_next_ticket'
$1 = 1491
(gdb) p '(anonymous namespace)::g_now_serving'
$2 = 1491
(gdb) finish
(gdb) p '(anonymous namespace)::g_next_ticket'
$3 = 1492
(gdb) p '(anonymous namespace)::g_now_serving'
$4 = 1491
```

The lock was free (next equals serving). CPU#1 drew ticket 1491 and, since serving is 1491, holds
it. Two cores are in the lock code at the same instant: CPU#2 is drawing its own ticket. The
large number counts every acquisition of the lock since reset.

**Why, and the contract:** [Book: The separate question of kernel exclusion](../book/one-lock-many-ready-queues.md#the-separate-question-of-kernel-exclusion),
[Reference: invariants, `caller-held-scheduler-exclusion`](../reference/invariants.md#scheduler--instance-state),
[Reference: architecture, Scheduler ("Exclusion")](../reference/architecture.md#scheduler-the-core-constraint)

### Stop 9: a wake chooses a core and hands the thread over
**Where:** `kernel/sched/sched.cc`, `wake_no_resched`, `pick_and_seat`, `place`, `publish_ready`, `hand_to`, `ring_stage`; `kernel/include/kickos/sched.h`, `sched_home_for`; `kernel/sched/policy_fifo_rr.cc`, `policy_place`

In `hello`, `pong` posts the semaphore `ping` waits on. On the reference walk the post readied
`ping` and the scheduler then compared priorities. Here the question has a second half: *which
core* should run `ping`?

`wake_no_resched` marks `ping` READY and gives it a first home with `sched_home_for`: the waker's
own core if `ping`'s affinity mask allows it. (A thread's affinity is the set of cores it may run
on; with no explicit mask it gets its task's default set, here all four cores.) Then
`pick_and_seat` runs the usual pass on this core. If this core keeps running something else,
`place` asks the policy where `ping` should go instead. `policy_place` reads each peer's
*level*, the priority it is running (one byte per core in `sched_out[core].level`), and picks a
core running strictly below `ping`. An idle core runs at the idle priority, so it wins.

Now the rule that makes per-core queues safe: **no core writes another core's ready queue.**
`publish_ready` links the thread directly only when the target is this core; otherwise
`hand_to` marks it HANDED and puts its pool index on a ring toward the target:

```cpp
        __attribute__((noinline)) void hand_to(Thread* t, uint32_t home)
        {
            Kernel& k = kernel();
            KICKOS_DEBUG_ASSERT(sched_placeable_on(t, home));
            KICKOS_DEBUG_ASSERT(k.threads.index_of(t) >= 0);
            t->queue_core = static_cast<uint8_t>(home);
            t->state.to<ThreadState::HANDED>();
            ring_stage(home, static_cast<uint16_t>(t - &k.threads.slots[0]));
            KICKOS_BENCH_SCHED_COUNT(::kickos::BS_HANDOFF);
        }

        // ...
        inline __attribute__((always_inline)) void publish_ready(Thread* t, uint32_t home,
                                                                  uint32_t me)
        {
            if (home == me)
            {
                link_ready(t, home);
                return;
            }
            hand_to(t, home);
        }
```

HANDED is a thread state that exists only above one core: runnable, but on no ready queue while
its ring entry travels. There is one ring per ordered pair of cores (`sched_slot[from][to]`), so
each ring has one writer and one reader. `ring_stage` only *stages* the entry; it is not visible
to the target yet.

**Try it:** `break kickos::(anonymous namespace)::hand_to`, then `ignore 1 3` and `continue` to
skip the boot-time spawns:

```
Thread 1 hit Breakpoint 1, kickos::(anonymous namespace)::hand_to (t=..., home=home@entry=1) at .../sched.cc
(gdb) bt
#0  kickos::(anonymous namespace)::hand_to (t=..., home=home@entry=1)
#1  kickos::(anonymous namespace)::publish_ready (t=..., home=1, me=0)
#2  kickos::(anonymous namespace)::place (t=t@entry=..., me=me@entry=0, held=...)
#3  kickos::(anonymous namespace)::pick_and_seat (woken=..., held=...)
#4  kickos::sem_post (s=...)
#5  (anonymous namespace)::syscall_body (nr=6, a0=2, a1=0, a2=0, a3=0)
#6  syscall_dispatch (nr=6, a0=2, a1=0, a2=0, a3=0)
#7  kickos_armv8a_el0_sync_entry () at .../switch.S
(gdb) p t->name
$1 = "ping", '\000' <repeats 11 times>
```

A `sem_post` on core 0 woke `ping`, core 0 kept its own thread, and placement sent `ping` to
idle core 1. Run `finish` (back into `place`) and `p t->state`: `kickos::ThreadState::HANDED`. The very first
`hand_to` of the run (no `ignore`) is root spawning `main` the same way, from `sched::add`.

**Why, and the contract:** [Book: A ready queue for each core](../book/one-lock-many-ready-queues.md#a-ready-queue-for-each-core),
[Reference: invariants, `no-pull`, `handed-is-named-once`, `ready-never-waits-while-a-core-below-idles`](../reference/invariants.md#scheduler--instance-state)

### Stop 10: publish, then ring the doorbell
**Where:** `kernel/sync/klock.cc`, `klock_leave`, `span_end`, `klock_resched_ask`; `kernel/include/kickos/instance.h`, `sched_flush`; `kernel/sched/sched.cc`, `sched_flush_owed`; `arch/arm64/common/arch_arm64_gicv2.cc`, `kickos_armv8a_gic_doorbell_send`

The staged entry is published when the waker's lock span ends. `klock_leave` releases the lock
at depth zero and calls `span_end`, which calls `sched_flush`: for every core this span staged
something toward, it advances that ring's `head` (making the entries visible) and asks that core
to reschedule. If the span booked a context switch, the same happens later, in
`kickos_switch_unlock`, once the outgoing thread is saved: a target must never link a thread
whose registers are still live on another core.

The ask itself is two steps, state first, then the interrupt:

```cpp
    void klock_resched_ask(uint32_t cores)
    {
        // Sets the cell before the raise, for peers alone: the raise is an edge a poll may
        // absorb instead of the vector, and the cell is what outlives it. A bare raise: a
        // reschedule owes no rendezvous answer.
        uint32_t const peers = cores & ~(1u << kickos_kernel_core());
        resched_owe(peers);
        arch_ipi_raise(peers);
    }
```

`resched_owe` bumps `g_asked[me].seq[to]`, a counter only this core writes. The interrupt can be
absorbed or merged with another (an SGI that is already pending stays one pending bit); the
counter cannot be lost. The target consumes it later by copying the value into its own
`g_took[to].seq[me]`. "Owed" means the two differ.

On arm64 `arch_ipi_raise` writes the GIC's `GICD_SGIR` register, which raises SGI 0 on every core
named in its target list, after a `dsb ish` so the ring writes are visible first.

**Try it:** stop at the first `hand_to` (no `ignore`), `delete`, then
`break kickos_armv8a_gic_doorbell_send`, `continue`:

```
Thread 1 hit Breakpoint 2, kickos_armv8a_gic_doorbell_send (cores=2) at .../arch_arm64_gicv2.cc
(gdb) bt
#0  kickos_armv8a_gic_doorbell_send (cores=2)
#1  kickos::sched_flush (me=<optimized out>) at .../instance.h
#2  kickos::(anonymous namespace)::span_end () at .../klock.cc
#3  kickos::klock_leave () at .../klock.cc
```

`cores=2` is binary `10`: core 1 only. The raise comes from `klock_leave`, after the lock is
released: a raise sent while still holding the lock would wake a core only to make it wait.

**Why, and the contract:** [Reference: invariants, `ring-published-at-span-end`, `sched-ring-cannot-fill`](../reference/invariants.md#scheduler--instance-state),
[Reference: porting, "The cross-core doorbell"](../reference/porting.md)

### Stop 11: the far side takes the doorbell
**Where:** `arch/arm64/common/arch_arm64_gicv2.cc`, `kickos_armv8a_gic_dispatch`; `arch/common/doorbell_protocol.cc`, `kickos_doorbell_service`, `arch_ipi_send`, `arch_ipi_wait`, `kickos_doorbell_boot_check`; `kernel/sched/sched.cc`, `kickos_kernel_core_resched_if_owed`, `ring_drain`, `ring_apply`, `push_take`

Core 1 is idle, asleep in `wfi`. The SGI wakes it into the IRQ vector, and
`kickos_armv8a_gic_dispatch` reads the GIC's acknowledge register. For SGI 0 it does two things,
in this order:

1. `kickos_doorbell_service`, *before* the end-of-interrupt write. This answers **rendezvous**
   requests: it compares `g_request[from].seq[me]` with `g_answer[me].seq[from]` for every other
   core, runs a barrier, does the requested work, and writes the answers. A rendezvous is used
   where a sender must know the far side *did* something: the boot check, gating an interrupt
   line routed to another core (`irq_line_op`), and the instruction barrier of Stop 12.
2. `kickos_kernel_core_resched_if_owed`, *after* the end-of-interrupt write, because it takes the
   kernel lock and a rendezvous initiator may be holding that lock while it waits for step 1.

One interrupt line, two meanings, and the counters say which one applies. A core may receive
the SGI with only a rendezvous owed, only a reschedule owed, or both.

`kickos_kernel_core_resched_if_owed` consumes the reschedule ask, takes the lock, drains its
inbound rings (at most `SCHED_DRAIN_BUDGET` entries, so a busy peer cannot keep it there), and
runs a normal scheduling pass:

```cpp
    if (not ::kickos::klock_resched_take())
    {
        return;
    }
    // ...
    ::kickos::IrqLock lock;
    ::kickos::KernelCore const me = kickos_kernel_core();
    if (::kickos::kernel().current(me) == nullptr)
    {
        return;
    }
    // ...
    bool const over = ::kickos::ring_drain(me, ::kickos::SCHED_DRAIN_BUDGET, true, lock);
    // ...
    ::kickos::push_take(me, lock);
    ::kickos::sched::reschedule(nullptr, lock);
```

`ring_apply` turns each entry back into a thread: it checks the HANDED thread may still run here,
marks it READY and links it on *this* core's queue. `reschedule` then picks it, and the switch
happens at the interrupt's exit, exactly as an interrupt-driven wake did in
[Sleep](03-sleep.md). `push_take` answers the other kind of request a core can receive: a peer
whose level dropped asking this core to push it a waiting thread. Threads only ever move by being
pushed, never pulled out of another core's queue.

**Try it:** stop at the first `hand_to`, `delete`, then `break kickos_kernel_core_resched_if_owed`
and `continue`:

```
Thread 2 hit Breakpoint 3, kickos_kernel_core_resched_if_owed () at .../sched.cc
  1    Thread 1.1 (CPU#0 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
* 2    Thread 1.2 (CPU#1 [running]) kickos_kernel_core_resched_if_owed () at .../sched.cc
(gdb) bt
#0  kickos_kernel_core_resched_if_owed () at .../sched.cc
#1  0xffffff8040019e98 in kickos_armv8a_irq () at .../arch_armv8a.cc
#2  0xffffff804001b838 in kickos_armv8a_irq_entry () at .../switch.S
#3  0xffffff804000b38c in kickos::(anonymous namespace)::idle_entry () at .../kmain.cc
```

Core 1 was interrupted in its idle thread. Now read the cells (the anonymous-namespace names
need quotes):

```
(gdb) p 'kickos::(anonymous namespace)::g_asked'[0].seq
$1 = {0, 1, 0, 0}
(gdb) p 'kickos::(anonymous namespace)::g_took'[1].seq
$2 = {0, 0, 0, 0}
(gdb) p kickos::detail::g_instance.per_instance[0].sched_out[0].head
$3 = {0, 1, 0, 0}
(gdb) p kickos::detail::g_instance.per_instance[0].sched_in[1].tail
$4 = {0, 0, 0, 0}
(gdb) p kickos::doorbell::g_request[0].seq
$5 = {0, 65, 65, 65}
(gdb) p kickos::doorbell::g_answer[1].seq
$6 = {65, 0, 0, 0}
```

(Each value prints as a nested `std::__atomic_base` struct; the number is its `_M_i`.) Core 0
asked core 1 once and core 1 has taken nothing: a reschedule is owed. Core 0's ring toward core 1
has head 1 and core 1 has drained to 0: one entry waits. The rendezvous cells agree, 65 asked and
65 answered, so this doorbell carries no rendezvous. The 65 is the boot check's 64 rounds (the
`rounds 0x40` on the console) plus the `arch_ipi_send` of Stop 6. Then
`break kickos::(anonymous namespace)::ring_apply`, `continue`, and
`p kickos::detail::g_instance.per_instance[0].threads.slots[entry].name` names the thread
arriving: `"main"`, state HANDED.

**Why, and the contract:** [Reference: porting, "The cross-core doorbell"](../reference/porting.md),
[Reference: invariants, `ring-published-at-span-end`, `reseat-carries-no-value`](../reference/invariants.md#scheduler--instance-state),
[design-multicore.md N4 (the doorbell serves both models)](../design-multicore.md)

### Stop 12: TLB maintenance across cores
**Where:** `arch/common/aspace_walk.h`, `invalidate_page_if`; `arch/arm64/armv8a/aspace_armv8a.cc`, `invalidate_page`, `invalidate_all`, `edits_done`, `instruction_side_rendezvous`, `arch_aspace_destroy`; `arch/arm64/armv8a/klock_armv8a.cc`, `kickos_arm64_instruction_side_rendezvous`

[The MMU chapter](10-the-mmu.md) showed a page unmapped and its TLB entry invalidated on the one
core. With four cores, every core that has run in that address space may hold a cached
translation of the page. Removing it from all of them is called a **TLB shootdown**. How much
work that takes depends on the architecture.

AArch64 does it in hardware. A TLB invalidate with the `IS` suffix (Inner Shareable) reaches every
core in the inner-shareable domain, and the `dsb ish` after it waits until all of them are done.
`invalidate_page`, shown in
[10, Stop 13](10-the-mmu.md#stop-13-unmap-and-tlb-maintenance), picks that form whenever
`KICKOS_KERNEL_CORES` is above 1.

So on arm64 an unmap needs no doorbell at all. One thing remains that the hardware broadcast
does not cover: a core may still execute instructions it already fetched from a page that is no
longer executable. Only an `isb` *on that core* discards them, and no instruction makes another
core execute one. So when an executable mapping is removed, `edits_done` calls
`instruction_side_rendezvous`, which sends a rendezvous to every other core that has used this
address space (the `Residency` record, kept per address space). Each target's doorbell service
runs `service_fence`, which is an `isb`.

**Try it:** use the `deadstack` image (same command line, `.../deadstack/deadstack`). Its
`reader` thread keeps reading another thread's stack while that thread exits and the kernel
unmaps the stack. `break arch_aspace_unmap`, `continue`, then
`break '(anonymous namespace)::invalidate_page_if'`, `set scheduler-locking on`, `continue`:

```
Thread 4 hit Breakpoint 2, (anonymous namespace)::invalidate_page_if (va=va@entry=1077575680, resident=resident@entry=true) at .../aspace_walk.h
  1    Thread 1.1 (CPU#0 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
  2    Thread 1.2 (CPU#1 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
  3    Thread 1.3 (CPU#2 [running]) 0x00000000401050c0 in (anonymous namespace)::reader () at .../deadstack/main.cc
* 4    Thread 1.4 (CPU#3 [running]) (anonymous namespace)::invalidate_page_if (...) at .../aspace_walk.h
(gdb) x/5i $pc
   ...:	tbz	w1, #0, ...
   ...:	dsb	ishst
   ...:	lsr	x0, x0, #12
   ...:	tlbi	vaae1is, x0
   ...:	dsb	ish
```

CPU#3 unmaps the dead thread's stack (`exit_current` -> `ustack_free` -> `arch_aspace_unmap`)
while CPU#2 is in user code in the same address space. One `tlbi vaae1is` covers both. The
console then shows `reader` killed with a data abort at the withdrawn page: the peer could not
keep reading through a stale translation.

**The contrast, on `qemu-riscv64-smp`.** RISC-V has no broadcast invalidate: `sfence.vma` only
affects the hart that executes it. So every edit that a peer's translations could depend on
needs a real software shootdown, `kickos_rv64_translation_rendezvous`, the same doorbell
rendezvous with a different fence in the far side:

```cpp
kickos::doorbell::Fenced kickos::doorbell::service_fence(Observed)
{
    __asm volatile("sfence.vma zero, zero" ::: "memory");
    return Fenced();
}
```

Launch `deadstack` from `build/qemu-riscv64-smp` with
`qemu-system-riscv64 -M virt -bios none -smp 4 -nographic -semihosting -kernel ... -gdb tcp::1234 -S`,
then `break kickos_rv64_translation_rendezvous if peers != 0`, `continue`:

```
Thread 1 hit Breakpoint 1, kickos_rv64_translation_rendezvous (peers=10) at .../klock_rv64imac.cc
#1  (anonymous namespace)::walk_map (root=..., va=2152132608, pa=<optimized out>, pages=3, ...) at .../aspace_walk.h
#2  arch_aspace_map (space=..., va=2152132608, pa=<optimized out>, pages=3, ...) at .../aspace_rv64imac.cc
#3  kickos::ustack_alloc (d=<optimized out>, want=want@entry=12288) at .../ustack.cc
```

`peers=10` is binary `1010`, harts 1 and 3. `delete`, `break kickos::doorbell::service_fence`,
`continue`, and both of those harts stop in the fence, each about to run its own `sfence.vma`.
x86_64 is the same shape as RISC-V: `invlpg` is local, and `peers_reload` in
`arch/x86/x86_64/aspace_x86_64.cc` makes every other online core reload CR3 through
`kickos_x86_64_translation_rendezvous`.

**Why, and the contract:** [Reference: porting, "The cross-core doorbell"](../reference/porting.md)
(the paragraphs on broadcast architectures and the instruction half)

### Stop 13: idle, per core
**Where:** `kernel/init/kmain.cc`, `idle_entry`, `idle_build`; `kernel/sched/policy_fifo_rr.cc`, `policy_pick_next`; `arch/arm64/armv8a/arch_armv8a.cc`, `arch_idle_wait`; `kernel/time/time.cc`, `ktime_rearm`

Each core has its own idle thread, built by the same `idle_build` as on the reference walk, with
an affinity of exactly its own core. `policy_pick_next` scans only this core's bitmap and, when
nothing on it may run here, returns `kernel().idle(core)`. Idle never travels on a ring and is
never placed elsewhere. Its body is the reference walk's, `arch_idle_wait` in a loop, which on
arm64 is a single `wfi`: the core stops until an interrupt is pending, whether its own timer or a
doorbell.

The timer is the one place where shared and per-core state meet. The sleep queue of
[Sleep](03-sleep.md) is one shared list under the kernel lock. But each core has its own timer
hardware, and `ktime_rearm` arms *this* core's timer, remembering the deadline in
`timer_armed_ns[core]`. So a sleeping thread is woken by whichever core's timer fires for its
deadline, and the wake then places it like any other (Stop 9).

**Try it:** with `hello` running, `break kickos::sched::wake`, `ignore 1 30`, `continue`, then
`info threads`:

```
* 1    Thread 1.1 (CPU#0 [running]) kickos::sched::wake (t=..., held=...) at .../sched.cc
  2    Thread 1.2 (CPU#1 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
  3    Thread 1.3 (CPU#2 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
  4    Thread 1.4 (CPU#3 [halted ]) 0xffffff8040019ec4 in arch_idle_wait () at .../arch_armv8a.cc
```

Then look at who is where (pool slot 2 is `ping`, slot 3 is `pong` in this image):

```
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) p t->name
$1 = "pong", '\000' <repeats 11 times>
(gdb) p $k->currents.cells[1]->name
$2 = "idle", '\000' <repeats 11 times>
(gdb) p $k->threads.slots[2].name
$3 = "ping", '\000' <repeats 11 times>
(gdb) p $k->threads.slots[2].queue_core
$4 = 1 '\001'
(gdb) p $k->threads.slots[3].queue_core
$5 = 0 '\000'
(gdb) p $k->threads.slots[3].state
$6 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) bt
#0  kickos::sched::wake (t=..., held=...) at .../sched.cc
#1  kickos::thread_abort_park (t=<optimized out>, result=<optimized out>, held=...) at .../park.cc
#2  kickos::ktime_on_timer () at .../held.h
#3  kickos_armv8a_gic_dispatch () at .../arch_arm64_gicv2.cc
#4  kickos_armv8a_irq () at .../arch_armv8a.cc
```

Both players sleep most of the time, so the other three cores sit in `wfi`, and `ping` and `pong`
have ended up homed on different cores. Core 0's own timer fired for `pong`'s deadline and is
waking it from the shared sleep queue. `p $k->sched_out[1].level.v_._M_i` reads `1`: the idle
priority 0, plus one (a zero means "not started").

**Why, and the contract:** [Book: The time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md),
[design-multicore.md section 8, "Idle needs no exemption"](../design-multicore.md)

### Same stops, other machines

The kernel half of every stop above (the lock's protocol, the rings, placement, the doorbell
cells) is the same code on every SMP target. What changes is in `arch/`:

| | `qemu-arm64-smp` | `qemu-riscv64-smp` | `qemu-x86_64-smp4` |
| --- | --- | --- | --- |
| Releasing a core | PSCI `CPU_ON` (`release_secondaries`) | every hart starts at reset and parks in `startup.S` (`.Lhart_park`) until its release word is set | INIT then startup IPIs through the local APIC (`kickos_x86_64_ap_start`) into `smp_trampoline.S` |
| Which core am I | MPIDR_EL1.Aff0 | `mhartid`, range-checked and claimed in `startup.S` | a core index in the per-CPU block reached through `%gs` |
| Doorbell | GIC SGI 0 (`kickos_armv8a_gic_doorbell_send`) | CLINT `msip` through a machine-mode trampoline, then `sip.SSIP` (`klock_rv64imac.cc`) | local APIC IPI (`kickos::x86_64::apic_doorbell_send`) |
| Kernel lock | ticket, `ldxr`/`stxr` draw | ticket, `amoadd.w` draw | CLH queue lock (`klock_x86_64.cc`): each waiter spins on its predecessor's cache line |
| TLB shootdown | hardware broadcast `tlbi ...is`, plus an `isb` rendezvous for executable pages | `sfence.vma` rendezvous | CR3 reload rendezvous |

The x86_64 image boots through UEFI firmware; [15. x86_64](15-x86_64.md) shows how to attach gdb
to it and walks how it starts the other three cores.

## Part 2: AMP

### Stop 14: what an AMP node is
**Where:** `Kconfig` (`KICKOS_AMP_POSTURE_SHARED_IMAGE`, `KICKOS_AMP_POSTURE_OWN_IMAGE`, `KICKOS_AMP_NODE_ID`); `boards/qemu-arm64/configs/amp2/defconfig`; `arch/common/amp_partition.ld.h`; `tools/amp/build-partition.sh`

In AMP each core runs **its own kernel**: its own thread pool, capability tables, scheduler and
timer, with `KICKOS_KERNEL_CORES` equal to 1. There is no kernel lock across cores and no
cross-core ready queue, because no kernel object is shared. The set of kernels on one machine
is called a **partition**, and each kernel is a **node**. Nodes may even run different
configurations, or on other parts different instruction sets.

KickOS builds a node in one of two postures:

- **Own image** (`qemu-arm64-amp2-n0` and `-n1`): one ELF per node, each linked at its own slice
  of RAM. The defconfig states the geometry once: `KICKOS_AMP_PARTITION_BASE`, the stride
  `KICKOS_AMP_NODE_SHARE` (64 MiB here), and `KICKOS_AMP_SHARED_SIZE` for a region above all
  slices that every node writes. `amp_partition.ld.h` derives every address from those numbers,
  so two images cannot disagree. The two builds differ only in `KICKOS_AMP_NODE_ID`, and
  `build-partition.sh` builds the peers and merges all images into one artefact to load.
- **Shared image** (`qemu-arm64-amp`): one ELF for all four cores, with the node index read from
  the core register. `InstanceLocal` (`include/kickos/instance_local.h`) keys every
  instance-scoped object on `arch_cpu_id()`, and `KICKOS_MAX_INSTANCES` provisions one `Kernel`
  per core. On this preset only core 0 runs a kernel. The other three are released as in Stop 3
  and stay in the doorbell park of Stop 5, where `kickos_armv8a_doorbell_park` runs the window
  service for them.

The rest of this part follows the own-image posture.

**Try it:** the gate builds the merged artefact with
`cmake --build build/qemu-arm64-amp2-n0 --target amp_partition` and boots it with the line
`ctest --test-dir build/qemu-arm64-amp2-n0 -N -V -R amp_partition` prints
(`-M virt,gic-version=3 -cpu cortex-a53 -nic none -smp 2 -m 132M`). The merge only joins the two
images' loadable segments, so you can also load the two builds directly: node 0's ELF as the
kernel, and node 1's flat `.bin` at its image base, 0x44000000. Node 1's ELF cannot be given to
the loader as well, because both ELFs describe the shared region at the same address.

```
B=build
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 -nic none -smp 2 -m 132M \
    -nographic -semihosting \
    -kernel $B/qemu-arm64-amp2-n0/user/apps/common/ampping/ampping_n0 \
    -device loader,file=$B/qemu-arm64-amp2-n1/user/apps/common/ampping/ampserve.bin,addr=0x44000000,force-raw=on \
    -gdb tcp::1234 -S &
gdb-multiarch $B/qemu-arm64-amp2-n0/user/apps/common/ampping/ampping_n0 \
    -ex "add-symbol-file $B/qemu-arm64-amp2-n1/user/apps/common/ampping/ampserve" \
    -ex 'target remote :1234'
```

Both kernels' symbols are now loaded. They have the same names, so a breakpoint on a kernel
function gets two locations, one per node: node 0's code is at `0xffffff8040...`, node 1's at
`0xffffff8044...`. Run without gdb and the console shows two banners, then:

```
ampping: node 1 serves port 3
ampping: 2 of 2 node app(s) alive on the port the partition names, own row 1
ampping: node 0 calls node 1 port 3
  serve 1 -> 2
  ping 1 -> pong 2 from node 1 (4 byte(s))
...
ampping: node 0 done, 4 round(s) across the partition
```

On the shared image (`hello` from `build/qemu-arm64-amp`, `-M virt,gic-version=3`),
stop anywhere and look at a peer core:

```
(gdb) thread 2
(gdb) bt
#0  0xffffff804001b334 in arch_idle_wait () at .../arch_armv8a.cc
#1  (anonymous namespace)::Part::sleep () at .../klock_armv8a.cc
#2  kickos::doorbell::park<(anonymous namespace)::Part> () at .../doorbell_protocol.h
#3  kickos_armv8a_doorbell_park () at .../klock_armv8a.cc
(gdb) p kickos::detail::g_instance.per_instance[1].currents.cells[0]
$1 = (kickos::Thread *) 0x0
```

**Why, and the contract:** [design-multicore.md N6, N6b and N6h](../design-multicore.md)

### Stop 15: a partition boots
**Where:** `kernel/init/kmain.cc` (the `KICKOS_AMP_NODE` block); `arch/include/kickos/arch/amp_shared.h`, `KICKOS_AMP_SHARED`, `arch_amp_shared_zero`; `kernel/amp/ampwindow.cc`, `window_init`; `arch/arm64/chip/virt_arm64/chip_virt_arm64.cc`, `arch_init`, `release_partition_peers`

Node 0 boots exactly as the one-core reference walk does, with these additions in `kmain`,
before root is created:

1. `amp::diag_peer_publish` and `amp::window_init`. The window is the shared memory of Stop 16.
   Anything both kernels write is declared with `KICKOS_AMP_SHARED("...")`, which puts it in a
   `.amp_shared.*` section that each node's link script places at the same derived address, as
   NOLOAD so no image's load overwrites a peer's writes. Node 0 alone zeroes that region
   (`arch_amp_shared_zero`), early in its reset path.
2. `arch_amp_release_peers`, which on this chip is `release_partition_peers`: the same PSCI
   `CPU_ON` as Stop 3, but the entry address is the peer's *image base*
   (`KICKOS_AMP_PARTITION_BASE + node * KICKOS_AMP_NODE_SHARE`) and the target core comes from
   `kickos_amp_node_core`. The peer then runs its own `_start`, its own C runtime and its own
   `kmain`. Node 0 releases peers only after the window is seated, so a peer can never publish
   into bytes node 0 is about to clear.
3. `amp_ports_seat`: the partition's crossings, listed once as `CONFIG_KICKOS_AMP_PORTS`
   (`"0:2,1:3,1:4"`: port 2 is served on node 0, ports 3 and 4 on node 1). Every node reads the
   same list. An entry naming *this* node becomes a local endpoint bound to that port; an entry
   naming another node becomes a **far endpoint** that root can call. The entry's position in
   the list is its capability index in root's table.

Some hardware belongs to the machine, not the node. `arch_init` writes the GIC distributor only
on the core with affinity zero, so a peer booting later does not reset lines node 0 is already
using.

**Try it:** with both images loaded as in Stop 14, `break release_partition_peers`, `continue`:

```
Thread 1 hit Breakpoint 1, (anonymous namespace)::release_partition_peers () at .../chip_virt_arm64.cc
* 1    Thread 1.1 (CPU#0 [running]) (anonymous namespace)::release_partition_peers () at .../chip_virt_arm64.cc
  2    Thread 1.2 (CPU#1 [halted ]) _start () at .../startup.S
```

CPU#1 has not run an instruction yet. `delete`, `break kickos::amp::window_init` (two locations,
one per image), `continue`:

```
Thread 2 hit Breakpoint 2.2, kickos::amp::window_init () at .../ampwindow.cc
  1    Thread 1.1 (CPU#0 [running]) kickos::(anonymous namespace)::ctz_word (v=<optimized out>) at .../frame.cc
* 2    Thread 1.2 (CPU#1 [running]) kickos::amp::window_init () at .../ampwindow.cc
(gdb) bt
#0  kickos::amp::window_init () at .../ampwindow.cc
#1  0xffffff80440094d4 in kickos::kmain (argc=<optimized out>, argv=<optimized out>) at .../kmain.cc
#2  0xffffff804401c724 in kickos_crt_tail () at .../crt_tail.h
```

CPU#1 is running the second kernel's own `kmain`, at `0xffffff8044...` addresses, while node 0
carries on with its own boot (here in the frame allocator).

**Why, and the contract:** [design-multicore.md N6g and N6h](../design-multicore.md)

### Stop 16: the shared window and its rings
**Where:** `kernel/include/kickos/ampwindow.h`, `Window`, `Ring`, `Slot`, `Class`; `kernel/amp/ampwindow.cc`, `g_window`, `send`, `send_on`, `take_call`, `take_reply`; `kernel/amp/ampmap.cc`, `ring`

The window is one array of rings: `inbox[class][to][from]`. For every ordered pair of nodes there
are two rings, one for **calls** and one for **replies**, because their slots are freed at
different times (a call slot is held until the reply is sent; a reply slot is freed once the
caller has copied it out). Each ring is a head written only by the sender, a tail written only by
the receiver, and `RING_SLOTS` fixed-size slots of up to `KOS_EP_MSG_MAX` bytes.

The header's opening comment is the rule for every field: anything a peer wrote is **untrusted
input**. A far index is never used as an index without a bound, a far length never as a length.
The receiver checks the head it reads; the sender checks the tail it reads:

```cpp
                uint32_t head = r.head.v.load();
                // FAR: the consumer owns this index. A producer that believed it would compute
                // a free-slot count out of it and overwrite slots the consumer is still
                // reading.
                uint32_t const tail = r.tail.v.load();
                // ...
                uint32_t const used = outstanding(head, tail);
                // ...
                // The slot index comes from the producer's OWN head, never from the far tail.
                Slot& s = r.slot[head & RING_MASK];
                s.port.store(port);
                s.tag = tag;
                s.len.store(len);
                if (len != 0u)
                {
                    kmemcpy(s.payload, payload, len);
                }
                // ...
                r.head.v.store(head + 1u);
                count_up(g_counts[me].sent);

                ring(to);
                return Sent::OK;
```

The head is published with a release store after the slot is complete, and `ring(to)` raises the
peer's doorbell: `amp::ring` maps the node to its core and calls `arch_ipi_send`, the same
doorbell as SMP. This validation defends a node against a *malformed* peer. It is not a security
boundary: every node maps the same writable kernel RAM, so a hostile peer kernel could rewrite
anything.

**Try it:** with both images loaded as in Stop 14, `break kickos::amp::send`, `continue`. Node 0's
`ampping` makes its first call:

```
Thread 1 hit Breakpoint 1.1, kickos::amp::send (to=1, port=3, tag=..., payload=..., len=len@entry=4) at .../ampwindow.cc
* 1    Thread 1.1 (CPU#0 [running]) kickos::amp::send (to=1, port=3, ...) at .../ampwindow.cc
  2    Thread 1.2 (CPU#1 [halted ]) 0xffffff804401ad54 in arch_idle_wait () at .../arch_armv8a.cc
(gdb) p 'kickos::amp::(anonymous namespace)::g_window'.inbox[0][1][0].head
$1 = {v = 0}
(gdb) p &'kickos::amp::(anonymous namespace)::g_window'
$2 = (kickos::amp::Window *) 0xffffff8048000140 <kickos::amp::(anonymous namespace)::g_window>
```

`inbox[0][1][0]` is the call ring (class 0) to node 1 from node 0, still empty. The window sits
above both images, at the same address in each. Then `delete`,
`break kickos::endpoint_far_call_deliver`, `continue`, and read the same ring from node 1's side:

```
(gdb) p 'kickos::amp::(anonymous namespace)::g_window'.inbox[0][1][0].head
$3 = {v = 1}
(gdb) p 'kickos::amp::(anonymous namespace)::g_window'.inbox[0][1][0].tail
$4 = {v = 0}
(gdb) p tag
$5 = (const kickos::amp::ReplyTag &) @0xffffff8044b24c78: {thread = 1, seq = 1}
```

One entry published by node 0, not yet taken by node 1. The tag names node 0's caller and its
call sequence.

**Why, and the contract:** [design-multicore.md N6f](../design-multicore.md),
[Reference: invariants, `amp-hold-landed-by-its-holder-or-given-back-at-exit`](../reference/invariants.md#scheduler--instance-state)

### Stop 17: a call that crosses kernels
**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_call` (the far arm), `endpoint_far_call_deliver`, `endpoint_far_reply_deliver`, `far_call_land`, `far_reply_land`; `arch/common/doorbell_protocol.cc`, `kickos_doorbell_service`; `kernel/amp/ampwindow.cc`, `kickos_amp_node_service`, `node_service`, `dispatch_call`

The `ampping` app on node 0 calls node 1 with plain `kos_call_timed`, the call of
[Call and reply](05-call-and-reply.md). Locality never reaches the API: the app does not know the
endpoint is far. In the kernel, `endpoint_call` resolves the capability as usual and then
branches:

```cpp
            if (far)
            {
                // ...
                uint16_t const seq = static_cast<uint16_t>(c->call_seq + 1u);
                amp::ReplyTag const tag = {kernel().threads.handle_for(idx),
                                           amp::reply_seq(seq)};
                int32_t const sent = far_publish(c, e, buf, send_len, tag);
                if (sent < 0)
                {
                    return sent;
                }
                c->call_seq = seq;
                reply_wait_seat(c, buf, recv_cap);
                // Cross-node calls do not donate priority to the peer scheduler.
                epoch = park_queueless(ask)(c, WAIT_EP_FAR_REPLY, e, lock);
                ktime_deadline_arm(c, timeout_us, lock);
                // ...
                sched::reschedule(nullptr, lock);
            }
```

The message goes onto the call ring with a **reply tag**: the caller's thread handle and a call
sequence number, which is how the reply finds its way back. No capability crosses between
kernels, only names. And no priority crosses either: two kernels' priority scales are not
comparable, so there is no priority inheritance across nodes.

On node 1 the doorbell enters `kickos_doorbell_service` (inlined into the GIC dispatch on this
build), which, after any rendezvous answers, calls `kickos_amp_node_service` -> `node_service`. That body drains the **reply rings first**
(a reply wakes a parked caller and must not wait behind a burst of calls), then the call rings,
each bounded per sender so a peer cannot keep the core in the masked handler. A call is handed to
`endpoint_far_call_deliver`, which wakes a receiver parked in `kos_reply_recv` on the bound
endpoint. The handler copies no payload: it hands the receiver a *hold* on the slot, and the
receiver copies its bytes out in its own syscall (`far_call_land`) with interrupts enabled. The
reply goes back on the reply ring the same way, `endpoint_far_reply_deliver` wakes node 0's
caller, and `far_reply_land` copies the reply into its buffer.

**Try it:** with both images loaded as in Stop 14, `break kickos::endpoint_far_call_deliver`,
`continue`:

```
Thread 2 hit Breakpoint 1.2, kickos::endpoint_far_call_deliver (from=0, port=3, tag=..., len=4, slot=0, held=...) at .../syscall_ipc.cc
  1    Thread 1.1 (CPU#0 [halted ]) 0xffffff804001ad44 in arch_idle_wait () at .../arch_armv8a.cc
* 2    Thread 1.2 (CPU#1 [running]) kickos::endpoint_far_call_deliver (...) at .../syscall_ipc.cc
(gdb) bt
#0  kickos::endpoint_far_call_deliver (from=0, port=3, tag=..., len=4, slot=0, held=...)
#1  kickos::amp::(anonymous namespace)::dispatch_call (me=1, from=0, ...) at .../ampwindow.cc
#2  kickos::amp::node_service (held=..., held@entry=...) at .../ampwindow.cc
#3  0xffffff804400df70 in kickos_amp_node_service () at .../held.h
#4  0xffffff804401a7b8 in kickos_armv8a_gic_dispatch () at .../arch_arm64_gicv3.cc
#5  0xffffff804401ad1c in kickos_armv8a_irq () at .../arch_armv8a.cc
```

Node 0's caller is parked, so CPU#0 sleeps; CPU#1 is in its doorbell interrupt. Then `delete`,
`break 'kickos::(anonymous namespace)::far_call_land'`, `continue`: CPU#1 again, now in the
server's own syscall (`endpoint_reply_recv`), copying the request out of the slot. Finally
`delete`, `break 'kickos::(anonymous namespace)::far_reply_land'`, `continue`:

```
Thread 1 hit Breakpoint 3.1, kickos::(anonymous namespace)::far_reply_land (c=..., buf=buf@entry=1077129192) at .../syscall_ipc.cc
(gdb) bt
#0  kickos::(anonymous namespace)::far_reply_land (c=..., buf=buf@entry=1077129192)
#1  0xffffff8040011170 in kickos::endpoint_call (cap=cap@entry=2, buf=buf@entry=1077129192, send_len=<optimized out>, recv_cap=4, timeout_us=500000) at .../syscall_ipc.cc
#2  (anonymous namespace)::syscall_body (nr=46, a0=2, a1=1077129192, a2=2052, a3=500000) at .../syscall.cc
(gdb) p c->name
$1 = "main", '\000' <repeats 11 times>
```

Node 0's `main` has woken inside the same `endpoint_call` that parked it, and copies the reply
into its buffer. Its `kos_call_timed` returns and the console prints
`ping 1 -> pong 2 from node 1 (4 byte(s))`: node 1 returned the round number plus one.

**Why, and the contract:** [Book: Synchronous call and reply](../book/synchronous-call-and-reply.md),
[Reference: IPC call and reply](../reference/ipc-call-reply.md),
[design-multicore.md N6d, N6e and N7](../design-multicore.md)

### Stop 18: sharing one console
**Where:** `arch/arm64/chip/virt_arm64/chip_virt_arm64.cc`, `g_console_claim`, `g_console_waiting`, `arch_console_write_retry`, `arch_console_claim_wait`; `arch/arm64/chip/virt_arm64/console_claim.h`, `Claim`; `kernel/include/kickos/sync.h`, `console_claim_wait`, `console_claim_rung`

Under SMP, the console is easy: there is one kernel, so all cores put their lines into one ring
and one drain sends them to the UART. Two AMP kernels each have their own ring, and two rings
cannot arbitrate one UART. So an own-image node writes the PL011 *polled*, and only while it
holds a **claim**: one 64-bit word in the shared region, `g_console_claim`, whose low half is the
owner (node index + 1) and whose high half is a deadline on the common counter.

`Claim::try_acquire` takes a free or expired claim with a compare-and-exchange (`ldaxr`/`stlxr`
in `compare_exchange`). The holder keeps the claim through a whole line, up to its newline, so two
nodes' lines never interleave mid-line. The claim is bounded: if a holder stalls, another node may
take it once the deadline has passed. A user thread whose write is refused does not spin. It marks
its node waiting in `g_console_waiting`, parks in `console_claim_wait`, and the holder's release
rings that node's doorbell; `kickos_amp_node_service` then calls `console_claim_rung`, which wakes
the parked writers. It is the same doorbell as the window's: each service drains the rings and
then checks for parked console writers.

**Try it:** with both images loaded as in Stop 14, `break kickos::virt_arm64::console::compare_exchange`,
`continue`. The first claim is node 0's, for its own banner, before node 1 is even released:

```
Thread 1 hit Breakpoint 1.1, kickos::virt_arm64::console::compare_exchange (expected=expected@entry=@0xffffff8043fffe58: 0, desired=14134827565449217) at .../chip_virt_arm64.cc
  2    Thread 1.2 (CPU#1 [halted ]) _start () at .../startup.S
(gdb) p/x desired
$1 = 0x32378d00000001
(gdb) bt 3
#0  kickos::virt_arm64::console::compare_exchange (...)
#1  kickos::virt_arm64::console::Claim::try_acquire (this=... <(anonymous namespace)::g_claim>, budget=budget@entry=3125000) at .../console_claim.h
#2  kickos::virt_arm64::console::Claim::open (this=... <(anonymous namespace)::g_claim>, budget=budget@entry=3125000) at .../console_claim.h
```

The word was free (`expected` is 0). The new value's low half is `1`, node 0 plus one; its high
half, `0x32378d`, is the deadline on the counter. `budget` is the 50 ms hold in counter ticks.
Then `delete`, `break kickos::console_claim_rung`, `continue`: it fires on CPU#1, called from
`kickos_amp_node_service` in node 1's doorbell interrupt. Without gdb, the two banners on the
console arrive whole, one after the other.

**Why, and the contract:** [Reference: console ("Across cores, the speaker is the device owner")](../reference/console.md)

## Where to next

- [12. ARMv6-M](12-armv6m.md): the next chapter, and the first of the per-architecture walks, on
  the smallest Arm cores.
- [The MMU](10-the-mmu.md), whose address spaces and TLB this chapter shared between cores.
- The per-arch chapters tell the bring-up and doorbell rows of the table above in full:
  [RISC-V](14-riscv.md) and [x86_64](15-x86_64.md).
- [Call and reply](05-call-and-reply.md) for the local half of the far call of Stop 17.
- The Book: [One kernel lock, many ready queues](../book/one-lock-many-ready-queues.md) for the
  design, and the Reference [scheduler invariants](../reference/invariants.md#scheduler--instance-state)
  for the exact rules each stop keeps.

## Exercises

1. **Pin a player.** In `user/apps/common/hello/main.cc`, keep the handle `create_caps` returns
   for `pong` and call `kos::thread::pin(handle.id(), 3)`. Predict: `pong` now always runs on core 3,
   and every wake from core 0 hands it over. Check: `break kickos::(anonymous namespace)::hand_to if home == 3`
   fires once per round, and `$k->threads.slots[3].affinity` reads `0x8`.
2. **Count the lock.** Note `g_next_ticket` at `kickos_kernel_core_start`, `continue` for a few
   ping-pong rounds, interrupt, and read it again. Predict how the rate changes if you build the
   one-core `qemu-arm64` preset instead. There, `info functions arch_kernel_lock` finds nothing:
   at one kernel core the lock calls compile away and `IrqLock` is the interrupt mask alone.
3. **Break the order.** Read `klock_resched_ask` and ask what would happen if `arch_ipi_raise`
   came before `resched_owe`. Then find the comment in `kickos::doorbell::poll` that explains why
   the poll raises the doorbell again when `kickos_kernel_core_resched_owed` is still true.
