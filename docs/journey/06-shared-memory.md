<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 6. Shared memory: arena blocks, grants, and the MPU commit

By the end of this chapter you will have followed a block of RAM from the moment a program asks
the kernel for it to the moment another thread writes into it, and you will have watched the
memory protection unit (MPU) refuse a write one byte past the end of what was granted. On the
way you will see where the block's bytes come from, how the kernel remembers who reserved it,
how a thread's set of regions is assembled and encoded into MPU descriptor words, and why those
words are written into the MPU only at the physical context switch and not when the scheduler
picks the next thread.

The ideas live in the Book. [Protection follows the CPU, not the scheduler's intent](../book/protection-follows-the-cpu-not-the-schedulers-intent.md)
is the main one: it explains why the commit sits where Stop 9 finds it.
[Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md) explains
the "memory posture" half of a thread's privilege, and
[The machine underneath](../book/the-machine-underneath.md) explains a memory map. The contract
is in [Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit).

## Two words before we start

An **MPU** is a small table in the CPU. Each row (a **region**, or **descriptor**) names a range
of addresses and what unprivileged code may do there: read, write, execute. An access that no
row allows faults. The Cortex-M4 in the `qemu` machine has a PMSAv7 MPU with 8 rows. A PMSAv7
row can only name a power-of-two size of at least 32 bytes, starting at an address that is a
multiple of that size. Remember that: it shapes every number in this chapter.

The MPU does not translate addresses. Every thread sees the same physical memory at the same
addresses, and the MPU only decides which of those addresses each thread may touch. So "sharing
memory" on this machine never means mapping a page somewhere new. It means adding a row to a
second thread's table that covers the same bytes.

That is also why this chapter has no **frame object**. On a part with page tables, KickOS hands
out physical frames as kernel objects (`kos_frame_create`), and a holder maps them into an
address space (`kos_frame_map`). Those calls are compiled only where `KICKOS_HAVE_ASPACE` is 1,
and on the `qemu` build it is 0: `info functions kos_frame_` in gdb finds nothing. Here the role
of the frame is played by an **arena block**: a run of RAM reserved with `kos_ram_alloc`. The
page-table version of the same story is [10. The MMU](10-the-mmu.md).

## The image we follow

We use the `mpu_fault` app (`user/apps/common/mpu_fault/main.cc`). It is short, and it does
exactly the three things this chapter is about:

```
    void* rA = kos_ram_alloc(BLOCK);
    // ...
    kos::thread::create(domainA_worker, rA, "domainA", 10, KOS_POLICY_FIFO, 0,
                        rA, REGION);
```

`main` reserves an 8 KiB block (`BLOCK`), then starts a thread called `domainA` and grants it
the low 4 KiB of that block (`REGION`, the last two arguments). The worker writes inside its
4 KiB, which works, then writes at `base + REGION`, the first byte of the high half, which
nobody granted it. That write faults, the kernel kills `domainA`, and the system carries on.

Two stops near the end use the `selftest` image instead, for the two other ways of sharing a
block: granting it to yourself, and granting it to a thread of another task.

```
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/mpu_fault/mpu_fault -gdb tcp::1234 -S &
gdb-multiarch build/qemu/user/apps/common/mpu_fault/mpu_fault \
    -ex 'target remote :1234'
```

### Stop 1: the app asks for memory
**Where:** `user/src/syscall_stubs.cc`, `kos_ram_alloc`; `kernel/syscall/syscall.cc`,
`syscall_body` (the `KOS_SYS_RAM_ALLOC` case)

`kos_ram_alloc` is a one-line stub that traps into the kernel with `KOS_SYS_RAM_ALLOC` (number
13), as [02](02-first-syscall.md) showed for every syscall. In the kernel the case has two
arms. The translating arm reserves a range of a task's address space; the region arm, the one
compiled here, is this:

```
            void* block = nullptr;
            {
                IrqLock lock;
                Thread* const c = sched::current();
                if (not cap_check_authority(c, AUTH_MEMORY))
                {
                    return 0;
                }
                // The block AND the record of who reserved it (ramown.h).
                block = ram_owner_alloc(c->task, static_cast<size_t>(a0));
            }
            // Cleared over the extent a region grants, with interrupts open: no other task can
            // name the block, and the arena never takes one back, so this is its only clear.
            if (block != nullptr)
            {
                ram_block_clear(block, arch_ram_region_size(static_cast<size_t>(a0)));
            }
            return reinterpret_cast<uintptr_t>(block);
```

Three things to notice. Reserving memory is an **authority** (`AUTH_MEMORY`), a power the task
was given at spawn, checked on the calling thread. The return value is a pointer, so there is no
room for an error code: every failure returns `NULL`. And the block is zeroed after the lock is
dropped, so a large clear does not hold off interrupts.

A reservation grants **no access**. `main` gets an address back, but its own MPU table has no row
for it, so `main` cannot read or write the block. The `mpu_fault` app relies on that: it never
touches the block itself.

**Try it:** `break kickos::ram_owner_alloc if size == 8192`, `continue`, `bt`. The first
`kos_ram_alloc` of the run is the init's 64-byte private block, which is why the condition is
there.

```
Breakpoint 1, kickos::ram_owner_alloc (owner=0x20006d80 <kickos::detail::g_instance+9664>, size=size@entry=8192) at .../kernel/mem/ramown.cc:55
#1  0x000074d2 in (anonymous namespace)::syscall_body (nr=13, a0=8192, ...) at .../kernel/syscall/syscall.cc:940
#2  syscall_dispatch (nr=13, a0=8192, ...) at .../kernel/syscall/syscall.cc:421
#3  0x0000242e in svc_trampoline () at .../arch/arm/armv7m/switch.S:540
```

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md),
[Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)
(the paragraph on arena confinement), the `kos_ram_alloc` comment in `user/include/kickos/sys.h`.

### Stop 2: where the bytes come from
**Where:** `arch/common/arch_ram_common.cc`, `arch_ram_alloc`; `arch/include/kickos/arch/arch.h`,
`arch_ram_region_size`

The bytes come from the **user-RAM arena**: one stretch of SRAM between two linker symbols,
`__kickos_ram_start` and `__kickos_ram_end`. The shared Cortex-M layout in
`arch/arm/common/cortexm.ld.h` places them, and the chip's linker script includes it
(`arch/arm/chip/mps2/mps2.ld`; on the f411, `arch/arm/chip/stm32f411/stm32f411.ld`). The
allocator is a **bump allocator**: one counter, `g_ram_used`, that only grows. Nothing is ever
given back to it. The kernel takes its own blocks from the same arena: the boot stacks before
any app runs, and later a default stack for each thread spawned without one. That is why the
counter is not zero when the first app asks.

Before it bumps, it rounds the request up to a size one MPU row can describe and aligns the base
to that size: the code is in [01, Stop 7](01-power-on.md#stop-7-the-arena-hands-out-the-first-two-stacks),
where the same allocator hands out the boot stacks.

On PMSAv7 `arch_ram_region_size` rounds up to the next power of two, with a floor of 32 bytes.
An 8 KiB request is already a power of two, so it stays 8 KiB, but it must start on an 8 KiB
boundary. A 16-byte request (you will see one at Stop 10) becomes 32 bytes. The rounding is
done here, at allocation, so a later grant of the whole block never has to round up into a
neighbour.

**Try it:** at the Stop 1 breakpoint, print the arena bounds and the counter, then `finish`:

```
(gdb) p/x (unsigned long)__kickos_ram_start
$2 = 0x20020000
(gdb) p/x (unsigned long)__kickos_ram_end
$3 = 0x203fc000
(gdb) finish
Value returned is $1 = (void *) 0x20028000
```

`0x20028000` is a multiple of `0x2000` (8 KiB): natural alignment at work.

**Why, and the contract:** [Book: Thread stacks](../book/thread-stacks-and-the-kiss-tension.md)
(why a no-free bump arena), [Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)
("Region budget").

### Stop 3: who may name the block
**Where:** `kernel/mem/ramown.cc`, `ram_owner_alloc` and `ram_owner_nameable`;
`kernel/include/kickos/ramown.h`

The bump allocator keeps no owner, and the MPU does not care who reserved what. So
`ram_owner_alloc` writes a small record beside the allocation: base, rounded size, and the
**task** that asked. A task is the group of threads that share one memory domain; ownership is
per task, so any thread of the reserving task may use the block, and a thread of another task
may not.

```
        RamBlock* const slot = free_block();
        if (slot == nullptr)
        {
            return nullptr;
        }
        void* const p = arch_ram_alloc(size);
        if (p == nullptr)
        {
            return nullptr;
        }
        // ...
        *slot = RamBlock{reinterpret_cast<uintptr_t>(p), static_cast<uint32_t>(rsz), tag};
```

The order matters. The free record slot is found before the arena is spent: since the arena
never takes a block back, allocating first and then finding the table full would lose the block
for the life of the image.

Later, every path that lets a caller name a range of RAM (a spawn's data grant, a caller-owned
stack, `kos_mem_self_grant`) asks `ram_owner_nameable`: is this range inside one block that the
caller's task reserved? The owner is stored as a task handle with a generation number, so when a
task dies its blocks become nameable by nobody, even if its slot is reused.

**Try it:** after the `finish` of Stop 2, print the record table:

```
(gdb) p '(anonymous namespace)::g_owners'.per_instance[0][1]
$2 = {base = 537034752, size = 8192, owner = 3}
```

`537034752` is `0x20028000`. Entry 0 is the init's 64-byte block.

**Why, and the contract:** [Reference: Invariants](../reference/invariants.md),
`arena-block-owner-is-the-reserving-task`.

### Stop 4: admitting the grant at spawn
**Where:** `kernel/syscall/syscall_thread.cc`, `spawn_grant_admit`; `kernel/thread/thread.cc`,
`ram_region_admit`; `kernel/grant/grant.cc`, `grant_region_admissible`

`kos::thread::create` with a `mem`/`mem_size` pair asks for a **data grant**: the new thread
gets read/write access to that range. The spawn path ([08](08-spawn-and-caps.md) walks it whole)
checks the range before it spends anything, in `ram_region_admit`:

```
        size_t const rsz = arch_ram_region_size(size);
        if (not arch_ram_region_admissible(base, rsz))
        {
            return -KOS_EINVAL;
        }
        // Rule 7 bounds the arena, not who inside it reserved what: a sibling task's block is
        // in-arena too.
        if (not grant_region_admissible(base, rsz, attr, false)
            or not ram_owner_nameable(granter->task, base, size))
        {
            return -KOS_EPERM;
        }
        // ...
        if (not memory_type_free(base, rsz, attr, except))
        {
            return -KOS_EBUSY;
        }
```

Read it as four questions, each with its own error code. Can one MPU row describe this range
exactly? (`-KOS_EINVAL`.) Is it inside the user arena, and clear of every block the kernel owns
for life, such as the MPU itself or the interrupt controller? That is "Rule 7",
`grant_region_admissible`. Did the caller's task reserve it? (Both `-KOS_EPERM`.) And is
nobody else holding the same bytes with a different memory type, cached in one place and
uncached in another? (`-KOS_EBUSY`.) Notice that the range is checked at its **rounded** size,
the size the MPU row will really cover, never at the size the caller typed.

**Try it:** `break kickos::ram_region_admit`, `continue`, then print the arguments and `finish`:

```
Breakpoint 2, kickos::ram_region_admit (granter=0x20005030 <...>, base=base@entry=537034752, size=4096, attr=attr@entry=3, how=how@entry=kickos::RAM_ADMIT_GRANT) at .../kernel/thread/thread.cc:338
(gdb) bt 3
#1  ... kickos::(anonymous namespace)::spawn_grant_admit (p=...) at .../kernel/syscall/syscall_thread.cc:509
#2  kickos::(anonymous namespace)::spawn_admit (p=...) at .../kernel/syscall/syscall_thread.cc:612
(gdb) finish
Value returned is $7 = 0
```

`attr=3` is `ARCH_MPU_R | ARCH_MPU_W` (`arch/include/kickos/arch/arch.h`: R is bit 0, W bit 1,
X bit 2, `ARCH_MPU_DEV` bit 3, `ARCH_MPU_NOCACHE` bit 4).

**Why, and the contract:** [Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)
("Rule 7 -- the grant path refuses kernel-reserved blocks"),
[Reference: Invariants](../reference/invariants.md), `grant-refuses-kernel-reserved-blocks`.

### Stop 5: the grant becomes a domain
**Where:** `kernel/task/task.cc`, `task_for`; `kernel/domain/domain.cc`, `domain_for`

A thread spawned with its own data grant does not join its parent's task. Admitting the grant
into the parent's task would hand the block to every other thread there too. So the spawn builds a
new task, and with it a new **domain**: the object that holds the regions every thread of the
task shares. On the region backend a domain is little more than one region:

```
        d->regions[0].base = base;
        d->regions[0].size = arch_ram_region_size(mem_size);
        d->regions[0].attr = attr;
        d->region_count = 1;
```

Building a task is itself an authority (`AUTH_TASKS`), checked in `spawn_masked` before
`task_for` runs.

**Try it:** `break kickos::domain_for if mem_size == 4096`, `continue`, `finish`:

```
#1  0x0000431c in kickos::task_for (caller=caller@entry=0, mem_base=mem_base@entry=0x20028000, mem_size=mem_size@entry=4096, ...) at .../kernel/task/task.cc:194
#2  0x00008cd0 in kickos::spawn_masked (...) at .../kernel/syscall/syscall_thread.cc:926
(gdb) finish
Value returned is $1 = (kickos::Domain *) 0x200066d0 <kickos::detail::g_instance+7952>
(gdb) p/x $->regions[0]
$2 = {base = 0x20028000, size = 0x1000, attr = 0x3}
```

**Why, and the contract:** [Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)
(a domain owns a region set; threads belong to a task).

### Stop 6: assembling the thread's region set
**Where:** `kernel/thread/thread.cc`, `thread_regions_stage` and `thread_regions_assemble`;
`kernel/include/kickos/mpuset.h`, `MpuSet`

An unprivileged thread has no default access to anything. Its whole view of memory is an
explicit list, built once at spawn, in this order:

```
        set.clear();
        if (not attr.privileged)
        {
            set.append_statics();
        }
        // ...
        if (dom != nullptr)
        {
            size_t const dn = domain_region_count(dom);
            for (size_t i = 0; i < dn; i++)
            {
                arch_mpu_region const* const dr = domain_region_at(dom, i);
                fitted = set.add(dr->base, dr->size, dr->attr) and fitted;
            }
        }
        if (not attr.privileged)
        {
            fitted = seat_windows(set, attr) and fitted;
        }
        if (not attr.privileged and stack_base != nullptr and stack_size != 0)
        {
            fitted = set.add(reinterpret_cast<uintptr_t>(stack_base),
                             arch_ram_region_size(stack_size), ARCH_MPU_R | ARCH_MPU_W)
```

First the **statics** (`arch_domain_static_regions` in `kernel/domain/domain.cc`): the code,
read and execute, and the app's static data, read and write. Then the task's domain regions.
Then the thread's own windows (Stop 11). Last its own stack. The stack is per thread, which is
why the MPU must be reloaded on every switch even between two threads of one task.

The result is an `MpuSet`. It holds two things side by side: the list of regions in a portable
form (`base`, `size`, `attr`), and the same list already **encoded** into the words the MPU
registers want (`image_`). Every function that changes the list re-encodes before it returns,
so the encoded image is never stale. The kernel uses the portable list too: when a syscall hands
the kernel a pointer, `user_range_ok` (`kernel/syscall/syscall_mem.cc`) checks it against these
same regions, so the kernel never accepts a pointer the thread itself could not use.

**Try it:** `break kickos::thread_regions_stage`. The first two hits are the boot's own threads;
a simpler way to land on the worker is to stop at Stop 1 first, `delete`, then
`break kickos::thread_regions_stage`, `continue`, `finish`:

```
Breakpoint 2, kickos::thread_regions_stage (dom=0x200066d0 <...>, attr=..., stack_base=stack_base@entry=0x2002a000, stack_size=stack_size@entry=8192) at .../kernel/thread/thread.cc:487
(gdb) finish
Value returned is $1 = (const kickos::MpuSet *) 0x20004e30 <kickos::detail::g_instance+1648>
(gdb) set $s = $
(gdb) p/x $s->regions_
$2 = {{base = 0x0, size = 0x400000, attr = 0x5}, {base = 0x20010000, size = 0x10000, attr = 0x3},
      {base = 0x20028000, size = 0x1000, attr = 0x3}, {base = 0x2002a000, size = 0x2000, attr = 0x3},
      {base = 0x0, size = 0x0, attr = 0x0}, ...}
(gdb) p $s->count_
$3 = 4 '\004'
(gdb) p/x $s->image_
$4 = {rbar = {0x0, 0x20010000, 0x20028000, 0x2002a000, 0x0, 0x0, 0x0, 0x0},
      rasr = {0x603002b, 0x1303001f, 0x13030017, 0x13030019, 0x0, 0x0, 0x0, 0x0}}
```

Four regions: code (4 MiB at 0, `attr = 5` is R|X), static data (64 KiB), the 4 KiB grant, and
the 8 KiB stack, which the spawn took from the arena right after the block (`0x2002a000`).
The other four rows are empty.

**Why, and the contract:** [Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)
("Region-set contract"), [Book: Privilege is three axes](../book/privilege-is-three-axes-not-one-bit.md).

### Stop 7: one region as two register words
**Where:** `arch/arm/common/arch_mpu_encode_default.cc`, `arch_mpu_encode` and `mpu_rasr`

A PMSAv7 row is two 32-bit registers. **RBAR** holds the base address. **RASR** holds the size,
the access permissions, the memory type, and an enable bit. `arch_mpu_encode` fills both for
each region, but only if `arch_mpu_region_encodable` says one row can describe it exactly;
otherwise the region gets no row at all, never a rounded one. The returned bitmask says which
regions got a row.

Take the grant's word, `0x13030017`, apart:

| Bits | Field | Value | Meaning |
|---|---|---|---|
| 28 | XN | 1 | never execute from here |
| 26:24 | AP | `011` | read/write for privileged and unprivileged code |
| 21:16 | TEX, S, C, B | `000011` | normal memory, write-back cached |
| 5:1 | SIZE | `01011` (11) | 2^(11+1) = 4096 bytes |
| 0 | ENABLE | 1 | the row is active |

The code region's `0x0603002b` has XN clear and AP `110` (read-only for everyone), and SIZE 21,
which is 4 MiB.

**Try it:** no new commands: compare `rasr[2]` (the 4 KiB grant, SIZE 11) and `rasr[3]` (the
8 KiB stack, `0x13030019`, SIZE 12) in the output of Stop 6.

**Why, and the contract:** [Book: The CPU reads ahead: memory types](../book/memory-types-and-speculative-access.md)
(why device and normal memory are different types), the ARMv7-M Architecture Reference Manual,
section B3.5 (the MPU registers).

### Stop 8: the scheduler decides, and only stashes
**Where:** `kernel/sched/sched.cc`, `switch_book`; `arch/common/arch_mpu_stash.cc`,
`arch_mpu_apply`

`main` returns from the spawn and parks, so the scheduler picks `domainA`. In `switch_book`, the
bookkeeping half of a switch, the kernel names the incoming thread's regions:

```
            // What the incoming thread may touch is installed here and nowhere else.
            [[maybe_unused]] struct arch_aspace* const rspace = aspace_activate_for(next);
            next->mpu.apply();
```

On Cortex-M that does **not** touch the MPU yet. `MpuSet::apply` calls `arch_mpu_apply`, which
only stores a pointer to the incoming thread's encoded image in one global cell:

```
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    g_pend_image = image;
}
```

The reason is the subject of the Book chapter. At this moment the **outgoing** thread (`main`,
still inside its syscall) is the one running on the CPU. The real switch of stacks happens later,
in the PendSV exception ([01, Stop 13](01-power-on.md#stop-13-the-first-pendsv-switches-to-root-and-turns-the-mpu-on)
and [03, Stop 8](03-sleep.md#stop-8-the-switch-happens-when-the-lock-opens) show it). If the MPU were
loaded now, `main` would run its last instructions under `domainA`'s regions, where its own stack
is not mapped, and fault on it.

**Try it:** from Stop 6, `delete`, then
`break arch_mpu_apply if image->rbar[2] == 0x20028000`, `continue`, `bt 4`:

```
Breakpoint 3, arch_mpu_apply (regions=0x20005204 <...>, n=4, image=0x20005268 <...>) at .../arch/common/arch_mpu_stash.cc:41
#1  0x000029ba in kickos::(anonymous namespace)::switch_book (next=0x20005188 <...>, me=0, held=...) at .../kernel/sched/sched.cc:430
#2  kickos::(anonymous namespace)::switch_to (me=0, held=..., next=0x20005188 <...>) at .../kernel/sched/sched.cc:534
#3  kickos::(anonymous namespace)::pick_and_seat (held=..., woken=<optimized out>) at .../kernel/sched/sched.cc:584
```

The image pointer `0x20005268` points into the new thread's own control block: the spawn copied
the staged `MpuSet` of Stop 6 into `Thread::mpu`.

**Why, and the contract:** [Book: Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Reference: Invariants](../reference/invariants.md), `mpu-apply-on-every-switch-in`.

### Stop 9: the physical switch commits
**Where:** `arch/arm/armv7m/switch.S`, `PendSV_Handler`;
`arch/arm/common/kickos_arch_mpu_commit_default.cc`, `kickos_arch_mpu_commit`;
`arch/arm/common/arch_arm_mpu_pmsav7.cc`, `kickos_arm_mpu_program`

PendSV saves `main`'s registers, loads `domainA`'s stack pointer and registers, sets
`CONTROL.nPRIV` for the incoming thread, and only then, as the last thing before the exception
returns, commits the stash:

```
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    pop     {r0, lr}
    cpsie   i                       /* pair consumed; never resume a thread masked */
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */
```

`kickos_arch_mpu_commit` reads `g_pend_image`, masks interrupts with `cpsid i`, and calls
`kickos_arm_mpu_program`. That function keeps a copy of the words the MPU already holds,
`g_mpu_held`, and writes only the rows that differ. For each changed row it selects the row
(`MPU_RNR`), disables it, writes the base, then writes the size and permissions, so the row is
never live with a new base and an old size:

```
static __attribute__((noinline)) void mpu_write_slot(uint32_t slot, uint32_t rbar, uint32_t rasr)
{
    reg32(MPU_RNR) = slot;
    reg32(MPU_RASR) = 0;
    reg32(MPU_RBAR) = rbar;
    reg32(MPU_RASR) = rasr;
}
```

After the writes it issues `dsb` and `isb`, so the next instruction, the exception return,
already runs under the new rows. `MPU_CTRL` stays at `ENABLE | PRIVDEFENA` the whole time.
`PRIVDEFENA` is the **background region**: privileged code (the kernel) may touch any address
the rows do not cover. That is how the kernel spends no rows on itself.

**Try it:** from Stop 8, `delete`, `break kickos_arm_mpu_program if img->rbar[2] == 0x20028000`,
`continue`, then look at the record before and after:

```
Breakpoint 4, kickos_arm_mpu_program (img=0x20005268 <...>) at .../arch/arm/common/arch_arm_mpu_pmsav7.cc:87
#1  0x000024d4 in kickos_arch_mpu_commit () at .../arch/arm/common/kickos_arch_mpu_commit_default.cc:36
#2  0x000022a2 in PendSV_Handler () at .../arch/arm/armv7m/switch.S:227
(gdb) p/x g_mpu_held[0]
$8 = {rbar = {0x0, 0x20010000, 0x20026000, 0x0, ...}, rasr = {0x603002b, 0x1303001f, 0x13030019, 0x0, ...}}
(gdb) finish
(gdb) p/x g_mpu_held[0]
$9 = {rbar = {0x0, 0x20010000, 0x20028000, 0x2002a000, ...}, rasr = {0x603002b, 0x1303001f, 0x13030017, 0x13030019, ...}}
```

Before: `main`'s rows (code, static data, `main`'s stack at `0x20026000`). After: rows 0 and 1
are the same for both threads and were not rewritten; rows 2 and 3 now hold the grant and
`domainA`'s stack. You can read the hardware itself: write the row number into `MPU_RNR`, then
read `MPU_RBAR` and `MPU_RASR`. (This changes `MPU_RNR`, which the next commit sets again
before it writes anything.)

```
(gdb) set *(unsigned *)0xE000ED98 = 2
(gdb) x/2wx 0xE000ED9C
0xe000ed9c:	0x20028002	0x13030017
(gdb) x/wx 0xE000ED94
0xe000ed94:	0x00000005
(gdb) info registers psp control
psp            0x2002bfe0          537051104
control        0x1                 1
```

`RBAR` reads back with the row number in its low bits. `MPU_CTRL` is 5: enabled, with the
background region. The process stack pointer is already inside `domainA`'s stack
(`0x2002a000` to `0x2002c000`), and `CONTROL` bit 0 says the thread will run unprivileged.

**Why, and the contract:** [Book: Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md)
("The fix: commit protection in the switch epilogue"), [Reference: Invariants](../reference/invariants.md),
`mpu-commit-writes-what-changed` and `mpu-bypass-windows-accepted`.

### Stop 10: granting a block to yourself
**Where:** `kernel/syscall/syscall.cc`, `syscall_body` (the `KOS_SYS_MEM_SELF_GRANT` case);
`kernel/thread/thread.cc`, `thread_self_grant`; `kernel/include/kickos/mpuset.h`,
`MpuSet::add_enforced_retyping`; `arch/common/arch_mpu_stash.cc`, `arch_mpu_apply_now`

A thread that reserved a block and wants to use it itself calls `kos_mem_self_grant`. The
`selftest` image does this in its `t_irqdrv` arm, which [07](07-interrupt-to-a-driver.md) walks:
`main` reserves 16 bytes to play a fake device register and grants them to itself.

`thread_self_grant` runs the same admission as Stop 4, then adds a row to the caller's own set.
If a row already names exactly this base and size, it is changed in place (a "retype", for
example from cached to uncached) rather than added twice. Then comes the one difference from a
spawn:

```
        // Must be effective BEFORE the return: the caller's next instruction may
        // dereference the region, and on a deferred-switch arch apply() only STASHES.
        // apply_now and NOT apply plus commit: a switch to another thread may already be
        // pended, and the pair would leave its epilogue the caller's image to program.
        c->mpu.apply_now();
```

There is no context switch here: the caller keeps running, and its next instruction may touch the
block. So `arch_mpu_apply_now` programs the MPU at once. It saves the stash cell, puts the
caller's image there, commits, and puts the old value back, all with interrupts masked, so a
switch that was already pending still finds its own thread's image in the stash.

**Try it:** run the `selftest` image (`.../user/apps/common/selftest/selftest`).
`break kickos::thread_self_grant if size == 16`, `continue`, `p/x c->mpu.regions_`, `finish`,
`p/x c->mpu.regions_`:

```
Breakpoint 1, kickos::thread_self_grant (c=c@entry=0x20005030 <...>, base=base@entry=537083744, size=size@entry=16, attr=attr@entry=3) at .../kernel/thread/thread.cc:371
(gdb) p/x c->mpu.regions_
$3 = {{base = 0x0, size = 0x400000, attr = 0x5}, {base = 0x20010000, size = 0x10000, attr = 0x3},
      {base = 0x20026000, size = 0x2000, attr = 0x3}, {base = 0x20033d00, size = 0x40, attr = 0x3},
      {base = 0x0, size = 0x0, attr = 0x0}, ...}
(gdb) finish
Value returned is $5 = 0
(gdb) p/x c->mpu.regions_
$6 = {{base = 0x0, size = 0x400000, attr = 0x5}, {base = 0x20010000, size = 0x10000, attr = 0x3},
      {base = 0x20026000, size = 0x2000, attr = 0x3}, {base = 0x20033d00, size = 0x40, attr = 0x3},
      {base = 0x20033f60, size = 0x20, attr = 0x3}, ...}
```

The 16 bytes became a 32-byte row, the smallest PMSAv7 can describe.

**Why, and the contract:** the `kos_mem_self_grant` comment in `user/include/kickos/sys.h`,
[Reference: Invariants](../reference/invariants.md), `mpu-apply-on-every-switch-in` (why the
stash is one cell).

### Stop 11: sharing a block with another task
**Where:** `kernel/thread/thread.cc`, `seat_windows` and `window_memory_attr`;
`kernel/syscall/syscall_mem.cc`, `window_get_call`; `system/init/compose/walk.cc`

A data grant (Stop 4) makes a new task. To let a thread of an **existing** task reach a block,
including a task other than the owner's, the spawner passes a **window**: an entry of the
`windows` list in `kos_thread_params`, with `kind = KOS_WINDOW_MEMORY`. A window belongs to the
one thread it was given to, not to its task. `seat_windows` turns each window into a row:

```
                size_t size = w.size;
                uint32_t rights = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV;
                if (w.kind == KOS_WINDOW_MEMORY)
                {
                    size = arch_ram_region_size(w.size);
                    rights = window_memory_attr(w.flags);
                }
                fitted = set.add_window(w.base, size, rights, static_cast<uint8_t>(i), w.flags)
                         and fitted;
```

A memory window takes the rounded size of its block and the rights its flags ask for:
`KOS_WINDOW_RO` drops the write bit, `KOS_WINDOW_UNCACHED` adds `ARCH_MPU_NOCACHE`. A device
window (`KOS_WINDOW_DEVICE`, the default kind) is never rounded, because rounding would hand the
holder the neighbouring device's registers; it gets `ARCH_MPU_DEV`, which encodes as device
memory. That is the window a userspace driver receives in [07](07-interrupt-to-a-driver.md). The
holder finds its windows with `kos_window_get`, which reads them back out of its own region set.

The same mechanism is how a whole system shares memory. The init (`system/init/compose/walk.cc`)
reserves each shared region of the composition with `kos_ram_alloc`, so the init's task owns it,
and hands it to every task that names it as a `KOS_WINDOW_MEMORY` window. A task finds it by
name with `kos_grant_mem`.

**Try it:** the `selftest` arm `t_window_memory_ro` spawns two children over one 32-byte block,
the first with a writable window, the second into another task with `KOS_WINDOW_RO`.
`break '(anonymous namespace)::t_window_memory_ro'`, `continue`, `delete`,
`break kickos::thread_regions_stage if attr.window_count == 1`, `continue`,
`p/x attr.windows[0]`, `finish`, `p/x $->image_`, and the same again for the second child:

```
$1 = {base = 0x20033f80, size = 0x20, kind = 0x1, flags = 0x0}
$5 = {rbar = {0x0, 0x20010000, 0x20033f80, 0x20028000, ...}, rasr = {0x603002b, 0x1303001f, 0x13030009, 0x13030019, ...}}

$6 = {base = 0x20033f80, size = 0x20, kind = 0x1, flags = 0x1}
$9 = {rbar = {0x0, 0x20010000, 0x20033f80, 0x20028000, ...}, rasr = {0x603002b, 0x1303001f, 0x12030009, 0x13030019, ...}}
```

The two rows differ in one hex digit: AP `011` (unprivileged read/write) became `010`
(unprivileged read-only). SIZE 4 is 2^5 = 32 bytes.

**Why, and the contract:** [Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)
("Cross-domain sharing = a region deliberately mapped into two domains"), the `kos_window_get`
and `kos_grant_mem` comments in `user/include/kickos/sys.h`,
[Book: Peripheral isolation and the hardware ceiling](../book/peripheral-isolation-and-the-hardware-ceiling.md)
(device windows).

### Stop 12: a stray access
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `HardFault_Handler` and
`kickos_armv7m_fault_report`; then `kickos_fault_kill_thread`

Back in `mpu_fault`, `domainA` writes `0x1111` at `base + 64`. Row 2 allows it. Then it writes
at `base + REGION`, `0x20029000`. No row covers that address, and an unprivileged thread gets
no background region, so the MPU refuses the store. The core takes a **MemManage** exception.
The `qemu` vector table routes MemManage, BusFault and UsageFault to the same handler,
`HardFault_Handler`, which picks the stack the frame was pushed on and jumps to the C reporter:

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

`kickos_armv7m_fault_report` first asks `kickos_fault_kill_thread` whether this fault can be
contained: an unprivileged thread faulted, so yes. The thread is killed, the rest of the system
keeps running, and the handler returns into another thread. Only a fault that cannot be
contained, one in the kernel itself, goes on to the panic dump. What the kill does, and who
survives it, is [9. Fault and kill](09-fault-and-kill.md).

**Try it:** run `mpu_fault` again. `break kickos_armv7m_fault_report`, `continue`, `bt 3`, then
read the fault registers: `CFSR` at `0xE000ED28` and `MMFAR` (the faulting address) at
`0xE000ED34`.

```
Breakpoint 1, kickos_armv7m_fault_report (frame=0x2002bfd8, exc_return=4294967293) at .../arch/arm/armv7m/arch_armv7m.cc:441
#1  <signal handler called>
#2  0x00000130 in (anonymous namespace)::domainA_worker (arg=0x20028000) at .../user/apps/common/mpu_fault/main.cc:41
(gdb) x/wx 0xE000ED28
0xe000ed28:	0x00000082
(gdb) x/wx 0xE000ED34
0xe000ed34:	0x20029000
```

`CFSR = 0x82` is two bits of its MemManage byte: bit 1, a data access violation, and bit 7, "the
address in MMFAR is valid". `exc_return = 0xfffffffd` says the frame is on the process stack, the
thread's own, at `0x2002bfd8`. `continue`, and the console shows the kill:

```
[domain] A: my region ok; writing domain B (expect fault)

=== THREAD FAULT === thread 'domainA' killed, system continues
  PC=0x130 CFSR=0x82
  ADDR=0x20029000
```

**Why, and the contract:** [Book: Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md),
[Reference: Invariants](../reference/invariants.md), `fault-exit-code-is-not-a-joiner-signal`.

## What changes with an MMU

On a part with page tables (`qemu-arm64`, `qemu-riscv64`, `qemu-x86_64`), the same calls exist and
mean the same thing, but every stop above changes underneath. The walk is
[10. The MMU](10-the-mmu.md); in short:

- **Stop 1-3:** `kos_ram_alloc` reserves a range of the task's own **address space**, backed by
  frames from the kernel's frame pool (`kernel/mem/frame.cc`, `kernel/mem/frame_pool.cc`), and
  the ownership record is the space's range list (`kernel/include/kickos/vrange.h`), not
  `ramown.cc`.
- **Frames become objects.** `kos_frame_create` mints a run of zeroed frames named by a
  capability, `kos_aspace_self` names the caller's address space, and `kos_frame_map` maps the
  run into any space whose capability the caller holds: true sharing between address spaces.
- **Stop 6-9:** there is no region list to encode. `aspace_activate_for` in `switch_book` installs
  the incoming thread's page-table root, and the protection is whatever the tables say. Grants
  are task-wide there, so another thread of the same task does reach a thread's stack: the per-thread stack row is a
  strength of the region backend only.
- **Stop 12:** a stray access is a translation or permission fault on the page tables, reported
  by the same `kickos_fault_kill_thread` path.

## Where to next

- [7. An interrupt to a driver](07-interrupt-to-a-driver.md): a device window and a self-granted
  block, put to work by a userspace driver.
- [8. Spawn and capabilities](08-spawn-and-caps.md): the rest of the spawn path that Stops 4 to 6 cut
  into.
- [9. Fault and kill](09-fault-and-kill.md): what happens after Stop 12.
- [10. The MMU](10-the-mmu.md): the same API over page tables.
- [13. ARMv8-M](13-armv8m.md): PMSAv8, where rows are base and limit instead of power-of-two
  sizes, and where two overlapping rows fault instead of the higher one winning.

## Exercises

1. Change `REGION` in `mpu_fault` to 3000. Predict what `ram_region_admit` sees as `rsz`, and
   whether the worker's second write still faults. Check with `break kickos::ram_region_admit`
   and by watching the console.
2. In Stop 9, rows 0 and 1 were not rewritten. Predict which rows a switch from `domainA` back to
   `main` rewrites. Check with
   `break kickos_arm_mpu_program if img->rbar[2] == 0x20026000` and `p/x g_mpu_held[0]` before
   and after `finish`.
3. Predict what happens if `main` writes into the block it reserved, without a self-grant.
   Which function reports it, and what does `MMFAR` hold? Add the write to a copy of the app and
   check with `break kickos_armv7m_fault_report`.
