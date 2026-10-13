<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 8. Spawn and capabilities: making threads and tasks, granting and resolving

By the end of this chapter you will have seen what a capability is in the code, an 8-byte
entry in a per-thread table, and where every entry in a running system came from. You will
follow the kernel as it gives the first thread its table and its authority, watch the init
hand a narrower authority down to the application, and then follow the application as it
creates an object, creates a task, spawns a thread into that task with one capability
granted, and closes its own copy. Along the way you will see every admission check a spawn
passes, the bump arena that thread stacks come from, and the frame the kernel fabricates so
a new thread can start.

The ideas live in the Book: [Authority comes from possession, or from identity](../book/authority-from-possession-or-from-identity.md)
explains why KickOS names objects by capability, [Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md)
explains handles and the resolve chokepoint, and [A stale handle must not be usable](../book/stale-handles-generation-width-and-allocation-policy.md)
explains the generation. [Thread stacks](../book/thread-stacks-and-the-kiss-tension.md)
covers where a stack comes from. The exact rules are in the
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc).

## The image we follow

We use `initdemo` (`user/apps/common/initdemo/main.cc`). Its composition
(`user/apps/common/initdemo/system.yaml`) starts one task, `main`, with the authorities
`memory, system, console, tasks`. Its `main` does, in order:

1. `kos_endpoint_create`: a new endpoint, and a capability to it in `main`'s own table.
2. `kos_ram_alloc` and `kos_task_create`: a stack block and an empty task.
3. `kos::thread::create_caps`: a thread called `sink`, in that task, on that stack, with one
   capability granted: the endpoint, narrowed to receive only.
4. `kos_console_publish`: stdout of every later thread goes to that endpoint.
5. `kos_handle_close`: `main` drops its own copy of the endpoint capability.
6. `kos::thread::create`: a thread called `worker`, whose `printf` now reaches `sink`.

So one short program creates an object, a task and two threads, grants a capability, and
closes one. The image exits with status 0 when `sink` has counted every byte `worker` printed.

Start it halted, as in [Setup](00-setup.md), and attach:

```sh
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/initdemo/initdemo -gdb tcp::1234 -S
gdb-multiarch build/qemu/user/apps/common/initdemo/initdemo -ex 'target remote :1234'
```

Most recipes below read kernel state through one convenience variable. The kernel keeps all
its state in one object, `kickos::detail::g_instance`, and `kernel()` returns its only
element on this build:

```
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
```

Remember the rule from the [README](README.md): gdb can only read kernel memory while the core
is stopped in kernel code. Every recipe here stops in a kernel function unless it says
otherwise.

Two decoding tables help with the output. `CapEntry::type` prints as a number:
0 empty, 1 semaphore, 2 mutex, 3 endpoint, 4 reply, 5 IRQ, 6 frame, 7 address space,
8 notification (`CapType` in `kernel/include/kickos/cap.h`). `CapEntry::rights` is a bit set:
1 `WAIT` (receive, or wait), 2 `SIGNAL` (send, or post), 4 `TRANSFER` (may be granted onward),
8 `HANDOUT` (an endpoint only, see Stop 8).

### Stop 1: a capability is an 8-byte entry
**Where:** `kernel/include/kickos/cap.h`, `CapEntry`, `KCAP_INDEX_BITS`, `CapType`, `CapRights`;
`system/include/kickos/sys/cap_rights.h`

A **capability** is the right to use one kernel object in some ways. In KickOS it is a slot
in a table that belongs to one thread. User code never holds a pointer to a kernel object.
It holds a **handle**, a 32-bit number that names a slot in its own table. The slot says
which object, what kind of object, and what this thread may do with it.

```
    struct alignas(8) CapEntry
    {
        // A full 32-bit generational handle: an object-pool one, or a THREAD one for a
        // CAP_REPLY. A DEAD entry holds the run's free-list links here instead
        // (kcap_free_link below).
        int32_t obj;
        uint8_t type : KCAP_TYPE_BITS;
        // CAP_REPLY's call sequence, or CAP_NOTIFY's badge; low half. cap_install_at seats
        // both halves from its own parameter, so no entry inherits the slot's last occupant.
        uint8_t seq_lo : KCAP_REPLY_SEQ_LO_BITS;
        uint8_t rights : KCAP_RIGHTS_BITS;
        uint8_t seq_hi : KCAP_REPLY_SEQ_HI_BITS; // the same field's high half
        uint16_t gen;                            // per-slot cap generation
    };
```

Read the fields one by one:

- `obj` names the object. It is not a pointer either: it is the object's handle in a global
  pool (one pool of endpoints, one of mutexes, and so on). That handle carries a second
  generation, the object's own, so a slot that still names an object freed by someone else
  stops resolving.
- `type` says which pool `obj` belongs to.
- `rights` are what the holder may do. A semaphore wait needs `WAIT`, a send needs `SIGNAL`.
- `gen` is the slot's own generation. It goes up by one each time the slot is emptied.
- `seq_lo`/`seq_hi` are spare bits. A notification capability keeps its badge there, and a
  reply capability its call sequence.

The handle user code holds is `(gen << 16) | index` (`KCAP_INDEX_BITS` is 16). A fresh slot
has generation 0, so on a fresh table the handle equals the index. That is why `initdemo`'s
`sink` can write `KOS_SPAWN_DELEGATED_CAP0` (the number 2) and mean "my slot 2".

Two slots are reserved in every table (`system/include/kickos/sys/cap_index.h`): slot 0,
`KOS_CAP_STDOUT`, the send-only console endpoint, and slot 1, `KOS_CAP_LIBC_LOCK`, the mutex
behind the C library's locks. Only the kernel fills them. Every other slot, from
`KOS_CAP_FIRST_DYNAMIC` (2) up, is the thread's own.

There is one more kind of power a thread can hold, and it is not in the table: the
**authority word**, `Thread::authority`. It is a bit set of what the thread may ask the
kernel to do that names no object: allocate memory (`KOS_AUTH_MEMORY`), create a task
(`KOS_AUTH_TASKS`), publish the console (`KOS_AUTH_CONSOLE`), shut down
(`KOS_AUTH_SYSTEM`), and four more. `cap_check_authority` tests it.

**Try it:** this needs no running target; gdb reads the layout from the ELF. Stops 2 and 3
show real entries.

```
(gdb) p sizeof(kickos::CapEntry)
$1 = 8
(gdb) ptype kickos::CapEntry
type = struct kickos::CapEntry {
    int32_t obj;
    uint8_t type : 4;
    uint8_t seq_lo : 4;
    uint8_t rights : 4;
    uint8_t seq_hi : 4;
    uint16_t gen;
}
```

**Why, and the contract:** [Book: Authority comes from possession, or from identity](../book/authority-from-possession-or-from-identity.md),
[Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)

### Stop 2: the table is a run of chunks from one slab
**Where:** `kernel/syscall/cap.cc`, `cap_slab_init`, `cap_slab_attach`; `kernel/include/kickos/cap.h`,
`CapRun`, `cap_slot`, `cap_run_free_build`

Every capability entry in the system lives in one static array, the **slab**
(`CapState::chunks` in `cap.cc`). At boot `cap_slab_init` cuts it into fixed-size
**chunks** of `KCAP_CHUNK_SLOTS` entries (8 on this build) and threads a free list through
the free chunks themselves: the first word of a free chunk points to the next one.

A thread's table is a **run**: a few chunks taken from that list, every one or none
(`CapChunkList::take`). `Thread::caps` is the run's directory, an array of chunk pointers,
and `cap_slot` turns an index into an entry with a shift and a mask. `cap_slab_attach`
reserves a run, zeroes it (a zeroed entry is an empty slot with generation 0) and calls
`cap_run_free_build`, which links every dynamic slot, from index 2 up, into a circular free
list.

That free list costs no memory of its own. A dead entry has no object to name, so its `obj`
word holds the list links: the next free index in the low half, the previous one in the high
half, both biased by one so that 0 can mean "none". `Thread::cap_free_head` is the head,
biased the same way.

Two widths matter. Root, the first thread, gets `KICKOS_MAX_HANDLES` slots (16 here, two
chunks). Every other thread gets `KICKOS_CAP_CHILD_WIDTH` (11 here, also two chunks, with the
tail of the second unused). The slab is sized at build time so that every thread slot that
could ever be in use has its run waiting: a spawn is never refused for want of a chunk.

The table never grows after the spawn. `cap_install` takes the head of the free list or
refuses with `-KOS_EMFILE`. That matters for servers: a client's call mints a reply
capability into the server's table, and a client must not be able to make the kernel
allocate on a server's behalf.

**Try it:** stop where the scheduler starts, before any thread has run, and look at root:

```
(gdb) break kickos::sched::start
(gdb) continue
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) set $r = &$k->threads.slots[0]
(gdb) p $r->caps
$1 = {chunk = {0x200071d8 <kickos::(anonymous namespace)::g_cap_all>, 0x20007218 <kickos::(anonymous namespace)::g_cap_all+64>}}
(gdb) p $r->cap_width
$2 = 16
(gdb) p $r->cap_free_head
$3 = 3
(gdb) p $r->caps.chunk[0][2]
$4 = {obj = 1048580, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0}
```

Two chunks, 64 bytes apart (8 entries of 8 bytes). The free head is 3, which is index 2
biased by one: slot 2 is the first free slot. Slot 2's `obj` is 1048580, which is
`0x00100004`: the previous free index is `0x10 - 1 = 15` (the last slot, since the list is
circular) and the next is `4 - 1 = 3`.

**Why, and the contract:** [Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
(the per-thread table, the chunk sizing, and the `KickOS: cap table =` configure line)

### Stop 3: what root holds when it starts
**Where:** `kernel/init/kmain.cc`, `kmain` (root's creation); `kernel/syscall/cap.cc`,
`cap_seat_authority`, `cap_libc_lock_take`, `cap_seat_libc_lock`

Root is the first thread. [Power-on](01-power-on.md) followed `kmain` building it; here we
look only at what it is given. Root is unprivileged from its first instruction, like every
thread but idle. Its power is entirely in its table and its authority word:

```
        // Ordering matters twice: after thread_create, which zeroes the cap table and would
        // wipe the seats; before sched::start(), so they are in place before root's first
        // instruction.
        {
            IrqLock lock;
            cap_seat_authority(root_tcb, CAP_AUTH_ALL);
            int const libc_lock = cap_libc_lock_take(root_tcb->task, lock);
            KICKOS_ASSERT(libc_lock != KCAP_LIBC_LOCK_NONE);
            cap_seat_libc_lock(root_tcb, libc_lock);
        }
```

So root holds every authority bit (`CAP_AUTH_ALL`), one capability at slot 1 (a fresh mutex,
the C library lock of its address space), and nothing else. Slot 0 stays empty: no console
has been published, and a thread whose slot 0 is empty prints through the kernel's own debug
console instead. Root holds no capability to any other object. Everything else in the system
is created later, by root or by the threads it starts.

Root never runs the application directly. Its entry, `kickos_root_entry`
(`user/src/root_entry.cc`), runs the app's global constructors and then the **init**,
`kickos_init_entry` (`system/init/compose/entry.cc`), which walks the composition table.

**Try it:** in the same session, still at the `kickos::sched::start` stop of Stop 2:

```
(gdb) p $r->name
$5 = "root", '\000' <repeats 11 times>
(gdb) p/x $r->authority
$6 = 0xff
(gdb) p $r->privileged
$7 = false
(gdb) p *$r->caps.chunk[0]@2
$8 = {{obj = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0},
      {obj = 0, type = 2 '\002', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0}}
```

Slot 0 is empty (type 0). Slot 1 is a mutex (type 2) with rights 0: the C library's lock and
unlock need no right, and with no `TRANSFER` it can never be granted to anyone.

**Why, and the contract:** [Book: Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md),
[Reference: invariants, `root-unprivileged-idle-alone-privileged`](../reference/invariants.md)

### Stop 4: the init hands authority down
**Where:** `system/init/compose/walk.cc`, `Walk::boot`, `Walk::start`, `Walk::spawn`, `Walk::release`;
`user/src/task_trampoline.cc`, `kickos::init::task_trampoline`

The init runs as root, unprivileged, and uses only system calls. `Walk::boot` sets up what
the init itself keeps for the life of the system: it lowers its own priority, creates a
notification object and binds itself to it (this is how it hears about deaths, see
[Fault and kill](09-fault-and-kill.md)), reserves a private block of RAM for its records,
and, for every task that serves an endpoint, creates that endpoint and narrows its own copy
to `SIGNAL | TRANSFER | HANDOUT` with `kos_cap_narrow`. The init never receives on a server's
endpoint, so it gives up `WAIT` at once.

Then `Walk::start` starts each task in the table. For `initdemo` there is one, `main`:

```
            check(kos_task_create(data, data_size, 0u, &handle), "kos_task_create", i);
            // ...
            rc = kos_task_sched_grant(handle, t.ceiling, t.core_mask);
            if (rc == 0)
            {
                s.step = "kos_task_watch";
                rc = kos_task_watch(handle, copy, r.endpoint);
            }
            if (rc == 0)
            {
                rc = take_lines(s);
            }
            if (rc == 0)
            {
                s.step = "kos_thread_create";
                rc = spawn(s);
            }
            release(s);
```

In words: create an empty task; give it a priority ceiling (12, from `ceiling:` in the
YAML); ask the kernel to raise a bit in the init's notification when that task ends, dies, or
becomes ready (`copy` is a badged copy of the notification, minted by `kos_notify_badge` just
above); claim any interrupt lines the task is granted; and spawn the task's first thread. `Walk::spawn` builds
the `kos_thread_params` from the table entry. Two fields carry the hand-down:

```
        p.authority = t.authority;
        p.task = s.handle;
```

`t.authority` is the composition's `authority:` list as bits, and the kernel refuses any bit
the spawner does not hold itself (Stop 8). The thread's entry is `task_trampoline`, which
calls `kickos_main`, which calls the application's renamed `main`. Finally `Walk::release`
closes the badged copy: the kernel's watch keeps its own record, so the init does not need
the capability any more.

**Try it:** `break kickos::spawn_masked`, `continue`. The first hit is the init spawning
`main` (the backtrace goes through `syscall_dispatch` from `svc_trampoline`):

```
(gdb) p *p
$1 = {entry = 0x1a75 <kickos::init::task_trampoline(void*)>, arg = 0xf800 <kickos_table_image+32>,
  name = 0xf838 <kickos_table_image+88> "main", prio = 2 '\002', policy = 0 '\000',
  cap_count = 0 '\000', quantum_ns = 0, mem_base = 0x0, mem_size = 0, windows = 0x0,
  window_count = 0, stack_base = 0x0, stack_size = 0, caps = 0x0, cap_dest = 0x0,
  core_mask = 0, authority = 113, task = 3}
```

113 is `0x71`: memory (1), system (16), console (32) and tasks (64). Root holds `0xff`; `main`
gets only the four bits its composition lists. Now `break kickos::handle_close`, `continue`:
the init closes handle 3, its badged copy.

```
(gdb) p c->caps.chunk[0][2]
$2 = {obj = 0, type = 8 '\b', seq_lo = 0 '\000', rights = 7 '\a', seq_hi = 0 '\000', gen = 0}
(gdb) p c->caps.chunk[0][3]
$3 = {obj = 0, type = 8 '\b', seq_lo = 1 '\001', rights = 7 '\a', seq_hi = 0 '\000', gen = 0}
(gdb) p $k->tasks[2]
$4 = {domain = 0x20006428 <kickos::detail::g_instance+7288>, refcount = 1 '\001', creator_tag = 1 '\001',
  gen = 0, prio_ceiling = 12 '\f', marks = 0 '\000', watch_bit = 0 '\000', sweeping = 0 '\000',
  watch_notify = 1, watch_ep = 0, exit_status = 130}
```

Slots 2 and 3 both name notification 0 (type 8). Slot 3 is the badged copy: its badge is
stored as bit + 1, so `seq_lo = 1` means bit 0. Task handle 3 names `tasks[2]` (the handle's
index is biased by one). That task has one member (`main`), was created by the thread whose
kill tag is 1 (root, slot 0), has its ceiling, and carries the watch.

**Why, and the contract:** [Reference: invariants, "App entry & init seam"](../reference/invariants.md#app-entry--init-seam),
[Book: Authority comes from possession, or from identity](../book/authority-from-possession-or-from-identity.md)

### Stop 5: an own-create takes the head of the free list
**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_create`; `kernel/syscall/cap.cc`, `cap_install`,
`cap_install_at`, `task_object_admit`

`main`'s first call is `kos_endpoint_create`. The syscall arm `KOS_SYS_ENDPOINT_CREATE` checks
the out-pointer (`cap_out_check`), then calls `endpoint_create`:

```
        if (not task_object_admit(CapType::CAP_ENDPOINT, c->task))
        {
            return -KOS_EAGAIN;
        }
        Endpoint* ep = nullptr;
        int const i = endpoint_claim_created(&ep);
        if (i < 0)
        {
            return -KOS_ENOMEM;
        }
        kernel().endpoint_refs[i] = 1;
        int const obj = kernel().endpoints.handle_for(i);
        int const rc = cap_install(c, obj, CapType::CAP_ENDPOINT, CAP_RIGHTS_ALL, out_cap);
```

Three checks, in order. `task_object_admit` counts the endpoints the caller's task already
holds and refuses at the task's budget, so one task cannot empty a pool that others need.
`endpoint_claim_created` takes a free pool slot. `endpoint_refs[i] = 1` is the creator's
reference: an object lives exactly as long as some capability names it. Then `cap_install`
puts a capability with every right into the caller's table:

```
        uint32_t const index = cap_run_peek_free(c->cap_free_head);
        if (index == KCAP_NO_SLOT)
        {
            return -KOS_EMFILE;
        }
        CapEntry const* const e =
            cap_install_at(c, static_cast<int>(index), obj_handle, type, rights,
                           KCAP_BADGE_NONE);
        *out_cap = (static_cast<uint32_t>(e->gen) << KCAP_INDEX_BITS) | index;
        return 0;
```

`cap_install_at` unlinks the slot from the free list and fills it. The handle is built from
the slot's current generation and its index. The handle goes back through an out-parameter,
not the return value, because a handle may use all 32 bits.

**Try it:**

```
(gdb) break kickos::endpoint_create
(gdb) continue
(gdb) break kickos::cap_install
(gdb) continue
(gdb) p c->name
$1 = "main", '\000' <repeats 11 times>
(gdb) p c->cap_free_head
$2 = 3
(gdb) finish
Value returned is $3 = 0
(gdb) p/x *out_cap
$4 = 0x2
(gdb) p $k->currents.cells[0]->caps.chunk[0][2]
$5 = {obj = 0, type = 3 '\003', seq_lo = 0 '\000', rights = 15 '\017', seq_hi = 0 '\000', gen = 0}
(gdb) p $k->currents.cells[0]->cap_free_head
$6 = 4
```

`main` gets handle 2: slot 2, generation 0. The entry names endpoint 0 (pool index 0,
generation 0) with all four rights (15). The free head moved on to index 3.

**Why, and the contract:** [Book: Adding a kernel object type](../book/adding-a-kernel-object-type-the-additive-recipe.md),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
("Lifecycle", and the own-create rule)

### Stop 6: a task is an empty group with a creator
**Where:** `kernel/syscall/syscall_thread.cc`, `task_create_call`, `task_create_admit`;
`kernel/task/task.cc`, `task_create`, `task_seat`, `task_handle`

A **task** is a group of threads that share one memory domain and live and die together: a
fault in one ends the whole task ([Fault and kill](09-fault-and-kill.md)). `main` wants
`sink` to be able to die without taking `main` along, so it creates a task for it.

`task_create_call` first runs `task_create_admit`, which needs `AUTH_TASKS` and, if a memory
grant is passed, checks the caller may grant it. Then `task_create` does the work:

```
        Domain* const d = domain_for(DOM_CALLER_TASK, mem_base, mem_size, mem_attr, donor, err);
        if (d == nullptr)
        {
            return nullptr;
        }
        Task* const t = task_seat(d, err);
        if (t == nullptr)
        {
            return nullptr;
        }
        // The hold, taken before the reservation is visible: an explicit task sits at
        // refcount 0 between create and its first spawn, and only this reference stops the
        // domain pool re-handing the slot underneath it.
        domain_ref(d);
        t->creator_tag = static_cast<uint8_t>(creator_tag);
        kernel().task_holds++;
```

`domain_for` picks the memory domain. `main` passes no memory, so on this region-MPU board
the task shares the default user domain (`domain_default_user`); on a board with page tables
it would get an address space of its own ([The MMU](10-the-mmu.md)). `task_seat` takes a free
task slot and keeps its generation. `creator_tag` records who created it: only that thread
may spawn into the task, watch it, or kill it (`task_created_by`). A thread's tag is its
pool index plus one. Back in `task_create_call`, `task_sched_inherit` copies the creator's
priority ceiling, and `task_handle` packs the slot index (plus one) and the generation into
the `kos_task_t` handle.

A task is not a capability. It is named by its own generational handle, and the authority to
use that name is "I created it", checked by tag.

**Try it:** the first `task_create_call` is the init's; the second is `main`'s.

```
(gdb) break kickos::task_create_call
(gdb) continue
(gdb) continue
(gdb) finish
0x00006d84 in (anonymous namespace)::syscall_body (nr=51, a0=0, a1=0, a2=537034632, a3=0) at .../kernel/syscall/syscall.cc:1248
Value returned is $1 = 0
(gdb) p/x h
$2 = 0x4
(gdb) p $k->tasks[3]
$3 = {domain = 0x20006428 <kickos::detail::g_instance+7288>, refcount = 0 '\000', creator_tag = 2 '\002',
  gen = 0, prio_ceiling = 12 '\f', marks = 0 '\000', watch_bit = 0 '\000', sweeping = 0 '\000',
  watch_notify = 0, watch_ep = 0, exit_status = 130}
```

`finish` lands in `syscall_body` (the source line gdb shows next to it is a neighbour that the
optimiser merged, ignore it). There `h` is the handle about to be delivered: 4, which is
slot 3. The task has no member yet (refcount 0), its creator tag is 2 (`main` is pool slot 1),
and its domain is the same one `main`'s own task uses. `exit_status` starts at 130
(`KOS_EXIT_CANCELLED`): a task whose members are all cancelled ends with that status.

**Why, and the contract:** [Reference: architecture, "Memory domains (the isolation unit)"](../reference/architecture.md#memory-domains-the-isolation-unit),
[Reference: porting, "Fault-isolation contract"](../reference/porting.md#fault-isolation-contract-a-faulting-thread-takes-its-task-and-nothing-beyond)

### Stop 7: where a stack comes from
**Where:** `kernel/syscall/syscall.cc`, the `KOS_SYS_RAM_ALLOC` arm; `kernel/mem/ramown.cc`,
`ram_owner_alloc`; `arch/common/arch_ram_common.cc`, `arch_ram_alloc`; `kernel/include/kickos/thread.h`,
`ThreadPool::stack_pop`, `ThreadPool::stack_push`

On a region-MPU board, all the RAM that user threads can be given comes from one **arena**:
the range between the linker symbols `__kickos_ram_start` and `__kickos_ram_end`
(`arch/arm/chip/mps2/mps2.ld`; on the f411, `arch/arm/chip/stm32f411/stm32f411.ld`).
`arch_ram_alloc` is a bump allocator over it. It rounds each request up to a size the MPU can
describe as one region and aligns the block to that size, because a PMSAv7 region must start
on a multiple of its own size. It never frees
([6. Shared memory, Stop 2](06-shared-memory.md#stop-2-where-the-bytes-come-from) has the details).

There are two ways a thread gets a stack from it:

- **The caller brings one.** `main` calls `kos_ram_alloc(SINK_STACK)` and passes the block to
  the spawn. The `KOS_SYS_RAM_ALLOC` arm checks `AUTH_MEMORY`, then `ram_owner_alloc` takes the
  block and records which task reserved it (`kernel/include/kickos/ramown.h`): a later grant
  of that block is admitted only to its owner. The block is cleared before it is returned.
- **The kernel picks one.** A spawn with `stack_base == nullptr` gets a block of
  `KICKOS_USER_STACK_SIZE` (8 KiB here). `spawn_masked` first tries the free list
  (`ThreadPool::stack_pop`, zeroing a recycled block), then `arch_ram_alloc`. Such a stack is
  marked `kstack_owned`, and when the thread's pool slot is reused (`ThreadPool::alloc`) the
  block goes back on that free list. One size class, so the list never fragments.

**Try it:** `break arch_ram_alloc` and `break kickos::ThreadPool::stack_pop`, then `continue`
eight times with `bt 2` at each stop. Trimmed to the caller frame:

```
Breakpoint 1, arch_ram_alloc (size=512)
#1  kickos::(anonymous namespace)::boot_stack_alloc (size=512, exhausted_msg=... "kmain: no arena for the idle stack")
Breakpoint 1, arch_ram_alloc (size=8192)
#1  kickos::(anonymous namespace)::boot_stack_alloc (size=8192, exhausted_msg=... "kmain: no arena for the root stack")
Breakpoint 1, arch_ram_alloc (size=64)
#1  kickos::ram_owner_alloc (owner=<optimized out>, size=64)
Breakpoint 2, kickos::ThreadPool::stack_pop (this=<optimized out>)
#1  kickos::spawn_masked (p=..., p@entry=0x20023e18, ...)
Breakpoint 1, arch_ram_alloc (size=8192)
#1  kickos::spawn_masked (p=..., p@entry=0x20023e18, ...)
Breakpoint 1, arch_ram_alloc (size=8192)
#1  kickos::ram_owner_alloc (owner=<optimized out>, size=8192)
Breakpoint 2, kickos::ThreadPool::stack_pop (this=<optimized out>)
#1  kickos::spawn_masked (p=..., p@entry=0x20027f98, ...)
Breakpoint 1, arch_ram_alloc (size=8192)
#1  kickos::spawn_masked (p=..., p@entry=0x20027f98, ...)
```

In order: idle's stack, root's stack, the init's private block (64 bytes), `main`'s stack
(picked by the kernel: on this build the thread pointer is found by masking SP, so the init
leaves every stack to the kernel, `Build::sp_masked` in `system/init/compose/walk.h`), `main`'s own
`kos_ram_alloc` for `sink`, and `worker`'s stack (picked by the kernel). Before each
kernel-picked stack, `stack_pop` finds the free list empty, so `arch_ram_alloc` follows:
nobody has died yet.

**Why, and the contract:** [Book: Thread stacks](../book/thread-stacks-and-the-kiss-tension.md),
[Reference: architecture, "Memory domains (the isolation unit)"](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 8: the spawn checks everything before it claims anything
**Where:** `kernel/syscall/syscall_thread.cc`, `spawn_masked`, `spawn_admit`, `spawn_stack_admit`,
`spawn_grant_admit`, `spawn_windows_admit`, `spawn_authority_admit`

`kos_thread_create` reaches `spawn_masked` (through `thread_create_call`). The whole spawn runs
under one `IrqLock`. Its first half only reads and checks. Nothing is claimed until every
check that can be done up front has passed, so a refused spawn leaves nothing behind.

1. **Copy the parameters in.** `spawn_params_in` checks the user pointer is readable and
   copies the `kos_thread_params` into a kernel local. From here on the kernel reads only its
   copy, so the caller cannot change a field after it was checked.
2. **`spawn_admit`.** The priority is in range (priority 0 is idle's alone). The stack, if
   given, is large enough, aligned, and (with the MPU enforcing) a block the caller may grant
   (`spawn_stack_admit`). The memory grant and device windows are admissible
   (`spawn_grant_admit`, `spawn_windows_admit`). Every authority bit asked for is one the
   caller holds (`spawn_authority_admit`): authority only narrows on the way down.
3. **The grant list.** Copied in one pass, then checked entry by entry:

```
            for (int ci = 0; ci < ncaps; ci++)
            {
                kos_cap_grant const g = gbuf[ci];
                CapEntry* se = cap_lookup(spawner, g.source_cap);
                if (se == nullptr)
                {
                    return -KOS_EBADF;
                }
                if ((se->rights & CAP_TRANSFER) != CAP_TRANSFER)
                {
                    return -KOS_EACCES;
                }
                uint8_t const mask = g.rights_mask;
                // Narrow-only, with one exception: an endpoint cap carrying the handout right
                // may seat CAP_WAIT in the child without holding it.
                uint8_t grantable = se->rights;
                // ...
                if ((mask & grantable) != mask)
                {
                    return -KOS_EACCES;
                }
```

   The source must be a live capability of the spawner, it must carry `TRANSFER`, and the
   child's rights are a subset of the parent's. The one exception is `HANDOUT`: a holder of an
   endpoint with `HANDOUT` may give `WAIT` (the right to receive) without holding it. That is
   how the init, which narrowed `WAIT` away in Stop 4, still starts a server on its endpoint.
   Each grant lands at child slot `KOS_SPAWN_DELEGATED_CAP0 + i` unless the caller names a
   slot, and two grants may not land on the same slot.
4. **The task.** A named task must resolve, must have been created by the spawner
   (`task_created_by`), and must not have ended. With no task named, the child joins the
   spawner's own task, unless it brings a memory grant, in which case the spawn builds a task
   for it (and needs `AUTH_TASKS`). The priority must not exceed the task's ceiling.

Only then does it claim: a thread slot (`ThreadPool::alloc`), a stack (Stop 7), the MPU region
set, a capability run of the child width (`cap_slab_attach`), a check of the grants against
the destination task's object budgets (`task_object_admit_grants`), a C library lock for the
child's address space (`cap_libc_lock_take`), and last, one reference per granted object
(`obj_ref_inc`). Each failure undoes what was claimed before it (`spawn_unwind`). The
reference bump is last because it is the one step whose undo would otherwise have to reach
into the finished child.

**Try it:** stop on the TCB build for `sink`, then return into `spawn_masked` to see what was
staged:

```
(gdb) break kickos::thread_create if $_streq(attr.name, "sink")
(gdb) continue
(gdb) p attr.cap_width
$1 = 11
(gdb) p attr.task->refcount
$2 = 0 '\000'
(gdb) p stack_base
$3 = (void *) 0x20028000
(gdb) p stack_size
$4 = 8192
(gdb) finish
kickos::spawn_masked (...) at .../kernel/syscall/syscall_thread.cc:1148
1148	        cap_install_defaults(child, lock);
(gdb) p deleg_dest[0]
$5 = 2
(gdb) p/x deleg_kind[0]
$6 = 0x31
(gdb) p/x deleg_obj[0]
$7 = 0x0
(gdb) p/d $k->endpoint_refs[0]
$8 = 2
```

The child's table is 11 wide. The task has no member yet. The stack is `main`'s own block.
The single grant goes to child slot 2. `deleg_kind` packs the kind and the rights in one byte:
`0x3` endpoint, `0x1` `WAIT`. It names endpoint 0, which now has two references: `main`'s and
the one taken for `sink`.

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md)
(validate, then use), [Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
(the B1 wire contract and delegation), [Reference: invariants, "Syscall, privilege & the user/kernel boundary"](../reference/invariants.md#syscall-privilege--the-userkernel-boundary)

### Stop 9: thread_create builds the TCB and a first frame
**Where:** `kernel/thread/thread.cc`, `thread_create`, `thread_regions_stage`, `tls_carve`;
`arch/arm/armv7m/arch_armv7m.cc`, `arch_context_init`

`thread_create` fills the thread control block (TCB) from the staged `ThreadAttr`. It zeroes
the TCB first, then seats the capability run and its free head (after the zeroing, or they
would be wiped), copies the name, sets the priority and policy, and takes a reference on the
task (`task_ref`): a thread holds its task for its whole life, and `task_ref` takes the
domain reference when the group goes from empty to one member.

On this board the MPU region set was staged by `spawn_masked` (`thread_regions_stage`) and is
copied in whole: the code and static data every unprivileged thread may use, the task's domain
regions, the thread's own device windows, and its own stack. The kernel then carves the
thread-local storage block off the low end of the stack and seats the thread's private kernel
stack (`ctx.kernel_sp`, one block per pool slot).

Last, `arch_context_init` fabricates the frame that the first context switch will "return"
into: exactly what the hardware would have stacked on an exception, plus the registers
`PendSV_Handler` saves. [01, Stop 8](01-power-on.md#stop-8-idle-and-root-are-fabricated-as-if-they-had-already-been-switched-out)
walks that code for root; `sink` gets the same frame. The return address is
`kickos_user_thread_return`: a thread whose function returns falls into the exit path.
`npriv = 1` makes the thread unprivileged.

**Try it:**

```
(gdb) break kickos::thread_create if $_streq(attr.name, "sink")
(gdb) continue
(gdb) break arch_context_init
(gdb) continue
(gdb) finish
(gdb) p/x t->ctx
$1 = {sp = 0x20029fbc, npriv = 0x1, resting_npriv = 0x1, stack_lo = 0x20028000,
  stack_hi = 0x2002a000, kernel_sp = 0x20000c30}
(gdb) x/17wx t->ctx.sp
0x20029fbc:	0x00000000	0x00000000	0x00000000	0x00000000
0x20029fcc:	0x00000000	0x00000000	0x00000000	0x00000000
0x20029fdc:	0xfffffffd	0x00000000	0x00000000	0x00000000
0x20029fec:	0x00000000	0x00000000	0x0000189b	0x00000134
0x20029ffc:	0x01000000
(gdb) p t->mpu
$2 = {static PLACE_SEATED = 8, regions_ = {{base = 0, size = 4194304, attr = 5},
  {base = 536936448, size = 65536, attr = 3}, {base = 537034752, size = 8192, attr = 3}, ...
```

Read the frame from the bottom up: eight zeros for r4-r11, `0xfffffffd` (EXC_RETURN), r0 = 0
(the argument), three zeros and r12, LR = `0x189b` (`kickos_user_thread_return`, Thumb bit
set), PC = `0x134` (`console_sink` with the Thumb bit cleared), and xPSR with only the Thumb
bit. The MPU set has three regions: flash, read and execute (`0x0`, 4 MiB); the shared static
data, read and write (`0x20010000`, 64 KiB); and the stack, read and write
(`0x20028000`, 8 KiB). [Shared memory](06-shared-memory.md) follows how those are committed at
the context switch.

**Why, and the contract:** [Book: Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md),
[Reference: invariants, "Context switch & FP save/restore"](../reference/invariants.md#context-switch--fp-saverestore)

### Stop 10: seat the slots, then make it runnable
**Where:** `kernel/syscall/syscall_thread.cc`, the end of `spawn_masked`; `kernel/syscall/cap.cc`,
`cap_install_defaults`, `cap_seat_libc_lock`, `cap_seat_authority`, `cap_install_at`;
`kernel/sched/sched.cc`, `sched::add`

After `thread_create` nothing may fail. The order is:

```
        thread_create(&k.threads.slots[i], p->entry, p->arg, stack, stack_size, attr);
        // Nothing below here may fail: the validation above guarantees each install succeeds,
        // and the reference loop already holds a reference for every cap seated here.
        Thread* const child = &k.threads.slots[i];
        cap_install_defaults(child, lock);
        cap_seat_libc_lock(child, libc_lock);
        cap_seat_authority(child, p->authority);
        for (int ci = 0; ci < ncaps; ci++)
        {
            cap_install_at(child, static_cast<int>(deleg_dest[ci]), deleg_obj[ci],
                           static_cast<CapType>(kcap_grant_type(deleg_kind[ci])),
                           kcap_grant_rights(deleg_kind[ci]), deleg_badge[ci]);
        }
        // ...
        sched::add(child, lock);
```

`cap_install_defaults` fills slot 0 with a send-only copy of the published console endpoint,
if one is published, and leaves it empty otherwise. `cap_seat_libc_lock` fills slot 1. The
authority word is seated from the (already checked) request. Each grant goes to its slot,
with the badge it had in the parent.

Then `sched::add` marks the child READY and links it into a ready queue. It does **not**
switch to it, even though `sink` (priority 12) outranks `main` (priority 2). Creation does not
preempt the caller; the new thread first runs when the caller blocks or yields (this is the
promise written above `kos::thread::create` in `user/include/kickos/kos.h`). That is why
`initdemo` can publish and close before `sink` ever runs.

**Try it:** watch the order of events. Run to `main`'s first call so the init's own spawns and
closes are behind you, then set four breakpoints and print the running thread's name at each
stop:

```
(gdb) break kickos::endpoint_create
(gdb) continue
(gdb) delete
(gdb) break kickos::sched::add
(gdb) break kickos::cap_console_publish_through
(gdb) break kickos::handle_close
(gdb) break kickos::cap_resolve_e if $_streq(c->name, "sink")
```

Then five times: `continue`, `p $k->currents.cells[0]->name`. Trimmed:

```
Breakpoint 2, kickos::sched::add (t=t@entry=0x20004fe0 <kickos::detail::g_instance+2096>, ...)
$1 = "main", '\000' <repeats 11 times>
Breakpoint 3, kickos::cap_console_publish_through (publisher=publisher@entry=0x20004e90 <...>, ...)
$2 = "main", '\000' <repeats 11 times>
Breakpoint 4, kickos::handle_close (c=0x20004e90 <...>, cap_handle=cap_handle@entry=2, ...)
$3 = "main", '\000' <repeats 11 times>
Breakpoint 2, kickos::sched::add (t=t@entry=0x20005130 <kickos::detail::g_instance+2432>, ...)
$4 = "main", '\000' <repeats 11 times>
Breakpoint 5, kickos::cap_resolve_e (c=c@entry=0x20004fe0 <...>, cap_handle=cap_handle@entry=2, ...)
$5 = "sink", '\000' <repeats 11 times>
```

`sink` (the TCB at `+2096`) is added first, but its first system call comes only after `main`
has added `worker` (the TCB at `+2432`) and gone to sleep.

**Why, and the contract:** [Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
(the B1 wire contract and the reserved slots), [Reference: architecture, "Scheduler (the core constraint)"](../reference/architecture.md#scheduler-the-core-constraint)

### Stop 11: the child uses its grant through the resolve
**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_reply_recv`, `endpoint_recv_locked`;
`kernel/syscall/cap.cc`, `cap_resolve_e`, `cap_lookup`

`sink` calls `kos_reply_recv` on handle 2 to wait for a message. The kernel turns that handle
into an endpoint through `cap_resolve_e`, the one function every syscall that names an object
goes through. [The first syscall, Stop 8](02-first-syscall.md) walks it line by line. In short:
`cap_lookup` checks that the index is inside this thread's table, that the slot is not empty,
and that the generation matches; `cap_resolve_e` then checks the type, then the rights
(`(rights & need) == need`), then asks the object pool, which checks the object's own
generation. The caller holds `IrqLock` from the resolve through the use, so no close can free
the object in between.

The point to notice here is whose table is searched: `c`, the calling thread's own. The same
number 2 names `main`'s endpoint capability in `main`'s table and `sink`'s narrowed copy in
`sink`'s table. Possession is per thread.

**Try it:**

```
(gdb) break kickos::cap_resolve_e if $_streq(c->name, "sink")
(gdb) continue
(gdb) p want
$1 = kickos::CapType::CAP_ENDPOINT
(gdb) p need
$2 = 1 '\001'
(gdb) p c->caps.chunk[0][2]
$3 = {obj = 0, type = 3 '\003', seq_lo = 0 '\000', rights = 1 '\001', seq_hi = 0 '\000', gen = 0}
(gdb) p/x c->authority
$4 = 0x0
(gdb) bt 4
#0  kickos::cap_resolve_e (c=c@entry=0x20004fe0 <...>, cap_handle=cap_handle@entry=2, want=want@entry=kickos::CapType::CAP_ENDPOINT, need=need@entry=1 '\001', ...)
#1  0x00007e0c in kickos::endpoint_recv_locked (ask=<synthetic pointer>..., c=0x20004fe0 <...>, cap=2, ...)
#2  operator() (__closure=<synthetic pointer>) at .../kernel/syscall/syscall_ipc.cc:1055
#3  kickos::endpoint_reply_recv (reply_cap=reply_cap@entry=4294967295, ...)
(gdb) finish
Value returned is $5 = (void *) 0x20004a90 <kickos::detail::g_instance+736>
```

A receive needs `WAIT` (1), and `sink`'s slot 2 carries exactly that. `sink` has no
authority at all: it was spawned with `authority = 0`.

**Why, and the contract:** [Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
("One resolve chokepoint")

### Stop 12: publishing the console fills slot 0 for later children
**Where:** `kernel/syscall/cap.cc`, `cap_console_publish_through`, `cap_console_publish`,
`cap_seat_stdout`, `cap_console_serve`, `cap_install_defaults`

`kos_console_publish(ep, sink_task)` needs `AUTH_CONSOLE`, and the capability it names must
carry `HANDOUT`. `cap_console_publish` then takes two references on the endpoint: one for
the kernel itself (the published target, so the endpoint outlives every thread's copy), and
one for the publisher's own slot 0, which `cap_seat_stdout` fills with a `SIGNAL`-only copy.
`cap_console_serve` records that `sink`'s task serves the console. A thread of that task gets
no stdout seat, or it would send its own prints to itself.

From now on, every spawn's `cap_install_defaults` seats slot 0 of the new child on this
endpoint. This is the "default capability set" a plain application gets without writing any
manifest: `worker` just calls `printf`, and the C library's `_write` sends on slot 0.

**Try it:**

```
(gdb) break kickos::cap_console_publish_through
(gdb) continue
(gdb) p e->rights
$1 = 15 '\017'
(gdb) p/d $k->endpoint_refs[0]
$2 = 2
(gdb) finish
Value returned is $3 = 0
(gdb) p/d $k->endpoint_refs[0]
$4 = 4
(gdb) p $k->currents.cells[0]->caps.chunk[0][0]
$5 = {obj = 0, type = 3 '\003', seq_lo = 0 '\000', rights = 2 '\002', seq_hi = 0 '\000', gen = 0}
```

`main`'s slot 0 is now a send-only (`SIGNAL`, 2) copy of endpoint 0, and the endpoint has two
more references. Later, at `worker`'s spawn:

```
(gdb) break kickos::cap_install_defaults
(gdb) continue
(gdb) finish
(gdb) p child->name
$6 = "worker\000\000\000\000\000\000\000\000\000"
(gdb) p child->caps.chunk[0][0]
$7 = {obj = 0, type = 3 '\003', seq_lo = 0 '\000', rights = 2 '\002', seq_hi = 0 '\000', gen = 0}
```

**Why, and the contract:** [Reference: console, "Capability handover"](../reference/console.md#capability-handover),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
("Low-barrier is a hard constraint")

### Stop 13: closing a capability
**Where:** `kernel/syscall/cap.cc`, `handle_close`, `obj_close_protocol`, `endpoint_rights_dropped`,
`cap_slot_vacate`, `obj_ref_drop`

`main` no longer needs its own copy: `sink` receives, and the kernel's published reference
keeps the endpoint alive. `kos_handle_close(ep)` reaches `handle_close`:

```
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -KOS_EBADF;
        }
        if ((cap_handle & KCAP_INDEX_MASK) < KICKOS_CAP_FIRST_DYNAMIC)
        {
            return -KOS_EPERM; // the kernel's seat
        }
        int const refused = obj_close_protocol(c, *e, /*teardown=*/false, held);
        if (refused != 0)
        {
            return refused; // protocol refused the close (owner closing a held mutex -> -KOS_EBUSY)
        }
        CapEntry const detached = *e;
        // Vacated BEFORE the ref drops, so no stale handle resolves during the drop, and
        // after the copy above, whose obj the vacate overwrites.
        cap_slot_vacate(c, cap_handle & KCAP_INDEX_MASK, e);
        obj_ref_drop(detached, /*teardown=*/false, held);
        return 0;
```

Four steps:

1. **Look up**, with the same `cap_lookup` as a resolve. A reserved slot cannot be closed.
2. **The type's close protocol** (`obj_close_protocol`). Most types have nothing to do. A mutex
   refuses the close if the caller owns it (`-KOS_EBUSY`: unlock first). An endpoint counts its
   receivers and its `HANDOUT` holders, so `endpoint_rights_dropped` takes this capability's
   rights off those counts; if it was the last receiver, every parked sender is answered at
   once rather than left waiting forever.
3. **Vacate the slot** (`cap_slot_vacate`): generation + 1, type empty, rights 0, and the slot
   goes to the **tail** of the free list. The tail, not the head, so a slot is reused as late
   as possible and its generation turns over slowly.
4. **Drop the reference** (`obj_ref_drop`): `refs - 1`, and the object is freed when it
   reaches 0. Destroy-on-last-close.

From the moment of step 3, handle 2 in `main` is stale. If `main` used it again,
`cap_lookup` would find generation 1 where the handle says 0, and answer `-KOS_EBADF`. If slot
2 is later reused, its new handle is `0x00010002`, not 2.

**Try it:**

```
(gdb) break kickos::handle_close if $_streq(c->name, "main")
(gdb) continue
(gdb) p c->caps.chunk[0][2]
$1 = {obj = 0, type = 3 '\003', seq_lo = 0 '\000', rights = 15 '\017', seq_hi = 0 '\000', gen = 0}
(gdb) p/d $k->endpoint_refs[0]
$2 = 4
(gdb) p/d $k->endpoints.slots_[0].recv_holders
$3 = 2
(gdb) p/d $k->endpoints.slots_[0].handout_holders
$4 = 1
(gdb) finish
Value returned is $5 = 0
(gdb) p $k->currents.cells[0]->caps.chunk[0][2]
$6 = {obj = 720900, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 1}
(gdb) p $k->currents.cells[0]->cap_free_head
$7 = 4
(gdb) p/d $k->endpoint_refs[0]
$8 = 3
(gdb) p/d $k->endpoints.slots_[0].recv_holders
$9 = 1
(gdb) p/d $k->endpoints.slots_[0].handout_holders
$10 = 0
```

Before: four references (`main`'s slot 2, `sink`'s slot 2, the kernel's published target,
`main`'s slot 0), two receivers (`main` and `sink`), one `HANDOUT` holder (`main`). After:
three references, one receiver (`sink`), no `HANDOUT` holder. The slot's generation is 1, its
`obj` now holds free-list links (`720900` is `0x000B0004`: previous index 10, next index 3),
and the free head is unchanged at index 3, because the released slot went to the tail.

**Why, and the contract:** [Book: Naming a kernel object, "Lifecycle"](../book/handles-and-the-resolve-chokepoint.md#lifecycle-refcount-and-destroy-on-last-close),
[Book: A stale handle must not be usable](../book/stale-handles-generation-width-and-allocation-policy.md),
[Book: Adding a kernel object type](../book/adding-a-kernel-object-type-the-additive-recipe.md)
(the close protocol before the reference drop)

### Stop 14: the other ways a capability changes hands
**Where:** `kernel/syscall/cap.cc`, `cap_narrow`; `kernel/notify/notify.cc` (`kos_notify_badge`);
`kernel/syscall/cap.cc`, `cap_install_reply`, `cap_teardown`

The spawn's grant list is the one path by which a thread copies a capability it holds into
another thread's table. The other ways a table changes are narrower:

- **Narrowing in place.** `kos_cap_narrow(cap, mask)` (`cap_narrow`) keeps `rights & mask`.
  It never widens. Dropping `WAIT` or `HANDOUT` from an endpoint capability moves the
  endpoint's counts exactly as a close of those rights would. With the pseudo-handle
  `KOS_CAP_AUTHORITY` it narrows the authority word instead. The init narrows its server
  endpoints this way (Stop 4).
- **A badged copy in your own table.** `kos_notify_badge` mints a second capability to the
  same notification with a badge, so the bit a holder raises says who raised it. The init uses
  one per task it starts.
- **A reply capability**, minted by the kernel into a server's table when a client calls
  (`cap_install_reply`). It names the waiting caller, not an object, and is used once.
  [Call and reply](05-call-and-reply.md) follows it.
- **Everything at once on exit.** `cap_teardown` closes every slot of a dying thread through
  the same protocol and reference drop as `handle_close`. [Fault and kill](09-fault-and-kill.md)
  follows it.

There is no system call that moves a capability between two running threads, and no message
carries one.

**Try it:** set `break kickos::cap_narrow` before the first `continue`. The init narrows only
the endpoints of tasks that serve one, and `initdemo`'s composition has none, so on this image
the breakpoint is never hit and the run ends with `[Inferior 1 (process 1) exited normally]`.
The badged copy is the one you already saw: slot 3 of root's table in Stop 4, `seq_lo = 1`.
To see a narrow, do exercise 1 below.

**Why, and the contract:** [Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
(the authority pseudo-handle, and "Service publication"), [Book: Synchronous call/reply](../book/synchronous-call-and-reply.md)

## What you have seen

Root starts with every authority bit, a C library lock, and no other capability. The init
creates objects, narrows its own copies, creates a task per composition entry, and spawns each
task's first thread with only the authority the composition grants. From there, an
application does the same on a smaller scale: it creates an object (one reference, one full
capability), creates a task (a group named by a handle and owned by its creator), and spawns a
thread whose grants are checked against the spawner's own table and can only narrow. The
kernel checks everything before it claims anything, claims in an order that can be undone, and
seats the child's slots only when nothing can fail any more. Closing reverses one capability:
protocol, then a new generation, then the reference.

## Where to next

- [9. Fault and kill](09-fault-and-kill.md): the next chapter, and the reverse of this one: a
  thread and a task torn down.
- [2. The first syscall](02-first-syscall.md), Stop 8: the resolve chokepoint in full.
- [5. Call and reply](05-call-and-reply.md): the reply capability, the one minted into
  someone else's table.
- [6. Shared memory](06-shared-memory.md): the MPU region set staged in Stop 9, committed at
  the switch.
- [10. The MMU](10-the-mmu.md): the same spawn on `qemu-arm64`, where a task gets its own
  address space and the stack comes from page frames (`ustack_alloc`).

## Exercises

1. **A grant without `TRANSFER`.** In `initdemo`'s `main`, call
   `kos_cap_narrow(ep, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_HANDOUT)` just before the
   `create_caps` call. Predict what the spawn answers and which check in `spawn_masked` says
   so. Check it with `break kickos::spawn_masked`, `finish`, and the returned value
   (`-KOS_EACCES` is -13).
2. **Widening is refused.** Change the grant mask to `KOS_CAP_WAIT | KOS_CAP_SIGNAL` and keep
   `main`'s full capability. Predict whether the spawn succeeds, then predict `sink`'s slot 2
   rights. Then narrow `main`'s capability to `KOS_CAP_SIGNAL | KOS_CAP_TRANSFER` first and
   predict again.
3. **A stale handle.** After `kos_handle_close(ep)`, call `kos_send(ep, "x", 1)`. Predict the
   answer, then stop in `cap_lookup` and print `cap_handle >> 16` and the slot's `gen` to see
   which comparison refuses it.
