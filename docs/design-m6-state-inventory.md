<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design -- kernel state, per-core versus genuinely global

> **Status: ON PAPER.** No code change. This is the classification a shared kernel needs, made
> while it is still cheap to make: `struct Kernel` is the inventory and `kernel()` is the single
> accessor, so the question is answerable by reading one header. It is produced in M5 and consumed
> in M6.

Companion to `design-m5-smp.md` (the staged model and the candidate ranking) and to
`design-capability-table.md` section 8 (the uniprocessor hazards in the capability path). Neither
of those carries an inventory; this does.

## 1. Why the inventory is not `struct Kernel`

`kernel/include/kickos/instance.h` holds 27 members and one accessor, and that is the part of the
answer that was already easy. **It is not the whole state.** Roughly twenty file-scope mutable
objects live outside the struct, and an inventory that reads only the struct misses every one of
them. `cap_slab` is the known example and it is far from the worst.

The classification below therefore has two halves, and the second half is where the work is.

## 2. `struct Kernel`, member by member

| Member | Class | Why |
|---|---|---|
| `ready[KICKOS_NUM_PRIO]` | per-core | one run-queue per core is the whole point of stage 2; global under the stage-1 big lock |
| `ready_bitmap` | per-core | derived from `ready`, moves with it |
| `current` | **per-core** | the canonical per-CPU word: each core runs a different thread |
| `idle` | **per-core** | each core needs its own idle thread |
| `live` | global | a whole-system liveness count, and shutdown is a system property |
| `boot` | **per-core** | each core has its own boot context to switch away from |
| `policy` | global | a vtable pointer; per-core policy is a feature nobody asked for |
| `next_tid` | global | thread ids must be unique system-wide, so this is a shared counter, not a replicated one |
| `trace_seq` | **per-core** | see ruling 2 |
| `trace_records_attempted` | **per-core** | see ruling 2 |
| `trace_dropped` | **per-core** | see ruling 2 |
| `trace_probe_overhead` | **per-core** | a measured cost, and on an asymmetric pair it genuinely differs |
| `task_holds` | global | counts tasks, and tasks are a system-wide pool |
| `sleepq` | **per-core** | bound to the per-core tickless timer |
| `sems`, `sem_refs` | global | object pools are the shared namespace; a cap minted on one core resolves on the other |
| `mutexes`, `mutex_refs` | global | same, and PI across cores requires one pool |
| `endpoints`, `endpoint_refs` | global | same, and cross-core IPC is the stated goal |
| `threads` (`ThreadPool`) | global | a TCB must be visible from any core |
| `domains` | global | shared region sets referenced by threads on either core |
| `tasks` | global | a task group spans cores by definition |
| `irq_table` | global | see ruling 1 |
| `irq_bindings`, `irq_refs` | global | cap-refcounted objects, same argument as the other pools |
| `irq_spurious_count` | global | a diagnostic counter; it is a shared read-modify-write and the lock is what serialises it |

Note `task_holds` sits where it does to occupy padding before `sleepq`, pinned by a
`static_assert`. `sleepq` going per-core moves that adjacency, so **splitting the struct along
this classification will move `microbit`'s arena base**, which is a build-level consequence, not a
cosmetic one: that board has no arena slack at all and any `.bss` movement costs a granule.

## 3. State outside `struct Kernel`

This is the half an inventory of the struct does not see. Classification as above.

### Kernel

| Object | Class | Note |
|---|---|---|
| `g_cap` (`kernel/syscall/cap.cc`) | global | the capability slab: chunks, free list, teardown depth. One capability namespace |
| `g_stdout_target` (same file) | global | one published console driver per system |
| `g_idle_tcb` (`kernel/init/kmain.cc`) | **per-core**, and it must move INTO `Kernel` first | the one thread the pool does not seat; its own comment already tags it instance-scoping residue |
| `g_fault` (`kernel/init/fault.cc`) | **per-core** | two cores can fault at once, and the record assumes one faulting thread |
| `g_tx` (`kernel/init/console_tx.cc`) | global | one physical UART; the ring is the arbitration point and needs a lock, not replication |
| `g_console_panicking` | global | a panic on either core must force every core polled |
| `g_console_state` | global | ownership of one device |
| `g_console_driver_died` | global | a property of one endpoint |
| `g_chip_writers` | global | **and it is the one to look at twice**: it counts writers ACROSS cores, which is what makes the drain-to-zero handshake mean anything, and a relaxed counter cannot carry that handshake. It needs release/acquire, not just atomicity |
| `g_handover_tried` | global | exactly-once reboot, system-wide |
| `g_led_on` | global | one LED |
| `g_kernel`, `g_default_user` (`kernel/domain/domain.cc`) | global | cached pointers into the global `domains[]` |
| the `KICKOS_BENCH` accumulators (`kernel/bench/`) | **per-core** | a shared accumulator across cores produces a number that describes nothing |
| `g_fifo_rr` (`kernel/sched/policy_fifo_rr.cc`) | neither | a `const` vtable. It is in `.data` for relocations, not because it is mutable |

### Arch

| Object | Class | Note |
|---|---|---|
| `g_arch_current`, `g_arch_next` (every backend) | **per-core** | the switch hand-off pair, and the prerequisite that gates even AMP |
| `g_isr_depth` / `g_in_isr` | **per-core** | interrupt nesting is a per-CPU property |
| `g_pend_regions`, `g_pend_count`, `g_fixed_count`, `mpu_ready`, `rgd0_ready` | **per-core** | the MPU/PMP is per-CPU hardware |
| `g_armed_deadline_ns` / `g_rx_armed_ns`, and the conversion caches beside them | **per-core** | follows the per-core tickless timer |
| `g_irq_masked`, `g_irq_pending`, `g_inject_line` | **per-core** | mirrors per-core interrupt-controller state. This, not `irq_table`, is the per-core half of interrupts (ruling 1) |
| the clock-extender pairs (`g_cyc_high`/`g_cyc_last` and the ten chip variants) | global data, broken exclusion | see ruling 3 |
| `SystemCoreClock`, `g_clint_msip`, the baud and clock-divider caches | global | set once at bring-up, read-mostly device facts |
| `g_ram_used` (`arch/common/arch_ram_common.cc`) | global | one arena; it needs a lock, not replication |
| `g_sim` (`arch/sim/sim.cc`) | per-instance | the arch-side twin of `Kernel`, and the second thing the multi-instance sim must key |
| `arch_init`'s `altstack` (`arch/sim/sim.cc`) | **per-host-thread, and it is a live bug for the multi-instance sim** | `sigaltstack` is a per-thread POSIX property. One shared 64 KiB alternate stack across N host threads corrupts the moment two of them fault concurrently |

### lib and system

`_SEGGER_RTT` and its three buffers (`lib/rtt.cc`) follow ruling 2. `kickos_init_args`
(`user/src/init_args.cc`) is per-instance: each simulated node wants its own argv. The driver-side
statics under `system/driver/` and `system/init/sim/` are userspace state inside a simulated node
rather than kernel state; they are as instance-scoped as everything else, and they are out of
scope for a `Kernel` inventory.

## 4. The three rulings

### Ruling 1 -- `irq_table` is GLOBAL; the per-core half is the arch mask state

The tempting answer is per-core, because the NVIC on both RP parts is per-core hardware with its
own enable and pending state. That confuses two different things.

- **Which handler serves line N is a logical fact**, and it is already cap-refcounted through a
  global `IrqBinding` pool. A per-core binding table would let one line have two different
  handlers, which is two answers to one question: the second truth the tiebreaker forbids.
- **Whether line N is enabled on THIS core is hardware state**, and it already lives in the arch
  layer (`g_irq_masked`, `g_irq_pending`), which this document classifies per-core.

So the decomposition already exists in the tree and needs no new split: the ISR on whichever core
takes the interrupt indexes one global table, and the per-core part is below the seam. That also
keeps the grant narrow, which is what the isolation principle asks for: a driver is granted a
LINE, not a line-on-a-core.

`irq_spurious_count` stays global for the same reason and is simply a shared counter under the
lock.

### Ruling 2 -- telemetry and RTT go PER-CORE, one up-channel each

The alternative is one shared stream with a lock on every record. Refuse it: the telemetry ring
exists to be single-writer and lock-free on the producer side, and putting a lock on the hottest
diagnostic path is exactly the thing it was built not to do. A shared stream would also make the
trace perturb what it measures more than the thing it measures.

Per-core costs a host-side demux and it costs nothing on the wire, because RTT natively carries
multiple up-channels: N cores is N up-channels in the existing control block, with no format
change. `trace_seq`, `trace_records_attempted` and `trace_dropped` become per-core and the host
sums them; `trace_probe_overhead` is per-core on its own merits, since on an RP2350 configured as
M33 plus Hazard3 the two cores are not the same machine.

The buffers themselves are the cost to watch, not the design: a second `up_buf` is real `.bss`,
and `KICKOS_RTT_CH1_SIZE` is already a configured knob. Per-core buffers must be sized per core
rather than duplicated at the current size.

### Ruling 3 -- the clock extenders are global data with an exclusion that SMP voids, and the fix is to delete the extender rather than lock it

Every backend that has only a 32-bit counter carries a `_high`/`_last` pair and extends it to 64
bits with a read-modify-write excluded by `arch_irq_save`. Masking local interrupts excludes
nothing on another core, so under a shared kernel that pair is unsynchronised, and it is the
sharpest of the three because a torn read there does not lose a count: it manufactures a phantom
2^32 jump that strands every timed wait.

Locking it is the wrong fix, and the tree already contains the right one. `xmc4800` chains four
CCU40 slices into ONE free-running 64-bit HARDWARE counter precisely so that "there is no software
wrap word, so no read can manufacture a wrap" -- and it did that to dodge an unreliable DWT, not
for SMP, which is why it is an existence proof rather than a plan. **Prefer a hardware 64-bit
counter on every chip that can build one.** Where the silicon genuinely cannot, a seqlock over the
pair is the fallback, and a per-core anchor is the other option `design-m5-smp.md` already names.

This is the same residue that document lists under the atomics conversion, and the reason it is
listed as ordering work rather than type work: the pair being two relaxed atomics instead of two
`volatile` words changes nothing about it.

## 5. What this classification costs, stated up front

Three consequences that are cheaper to know now than to discover during M6.

- **Splitting `Kernel` moves `microbit`'s arena base**, per the `task_holds`/`sleepq` adjacency
  assert. That board's `_ebss` IS its arena base, so any movement costs a full 32-byte granule and
  flips an arm. It is caught by a build, not silently.
- **`g_idle_tcb` has to move into `Kernel` before it can be made per-core**, and it is outside the
  struct today. That is a small change and it should be made in M5 rather than M6, because it is
  correct on its own terms: the idle TCB is instance state sitting outside the instance.
- **The sim's `altstack` is a bug now**, not an SMP hazard. It blocks the multi-instance sim the
  moment two host threads exist, and the multi-instance sim is an M5 requirement rather than an M6
  one.
