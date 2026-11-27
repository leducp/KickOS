<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 8. Spawn and capabilities: making threads and tasks, granting and resolving

By the end of this chapter you will have seen what a capability is in the code, a 16-byte
entry in a per-thread table, and where every entry in a running system came from. You will
follow the kernel as it gives the first thread its table and its authority, watch the init
hand a narrower authority down to the application, and then follow the application as it
creates an object, starts a task from a declaration with one capability granted, and narrows
and closes its own copies. Along the way you will see every admission check a spawn passes,
where thread stacks come from, and the frame the kernel fabricates so a new thread can start.

The ideas live in the Book: [Authority comes from possession, or from identity](../book/authority-from-possession-or-from-identity.md)
explains why KickOS names objects by capability, [Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md)
explains handles and the resolve chokepoint, and [A stale handle must not be usable](../book/stale-handles-generation-width-and-allocation-policy.md)
explains the generation. [Thread stacks](../book/thread-stacks-and-the-kiss-tension.md)
covers where a stack comes from. The exact rules are in the
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc).

## The image we follow

We use `initdemo` (`user/apps/common/initdemo/main.cc`). Its composition
(`user/apps/common/initdemo/system.yaml`) declares two tasks. `main` runs at priority 2 with a
priority ceiling of 12 and the authorities `memory, system, console, tasks`. `sink` is a
packaged console driver, `appsink` (`user/apps/common/sinkdrv/appsink.cc`), at priority 12. It
serves `/svc/console` and starts only when another task asks for it (`start: on_request`). The
init starts `main`. `main` then does, in order:

1. a `printf` before any console is published: its slot 0 is empty, so the line goes to the
   kernel's own debug console.
2. `kos_endpoint_create`: a new endpoint, and a capability to it in `main`'s own table.
3. `kos_program_find` and `kos_task_create`: an instance of the declared task `sink`, with one
   capability granted, the endpoint with `WAIT | TRANSFER`. The declaration serves the console,
   so the create also publishes the console through `main`'s endpoint capability.
4. `kos_handle_close` of the program capability, and `kos_cap_narrow` of the endpoint:
   `main` gives up `WAIT`, so `sink` is the only receiver.
5. `kos::thread::create`: a thread called `worker`, whose `printf` now reaches `sink`.
6. `kos_call` on the endpoint: `sink` answers how many bytes it counted.

So one short program creates an object, starts a task, spawns a thread, grants a capability,
and narrows and closes its own. The image exits with status 0 when `sink` has counted every
byte `worker` printed.

Start it halted, as in [Setup](00-setup.md), and attach:

```sh
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/initdemo/initdemo -gdb tcp::1234 -S
gdb-multiarch build/qemu/user/apps/common/initdemo/initdemo -ex 'target remote :1234'
```

Most recipes below read kernel state through two convenience variables. The kernel keeps its
state in one object, `kickos::detail::g_instance`, and `kernel()` returns its only element on
this build. The pools whose size the composition chooses (threads, tasks, endpoints, the
capability slab) sit in storage the composer generates, which `kickos::kickos_kernel_pools`
describes. Set both at your first stop in kernel code:

```
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) set $P = &kickos::kickos_kernel_pools
```

Then `$P->threads[i]` is thread slot `i`, `$P->tasks[i]` task slot `i`, and
`$P->endpoints.slots[i]` and `$P->endpoints.arrays.refs[i]` are endpoint `i` and its reference
count. `$k->currents.cells[0]` is the running thread.

Remember the rule from the [README](README.md): gdb can only read kernel memory while the core
is stopped in kernel code. Every recipe here stops in a kernel function unless it says
otherwise.

Two decoding tables help with the output. `CapEntry::type` prints as a number:
0 empty, 1 endpoint, 2 reply, 3 IRQ, 4 frame, 5 address space, 6 notification, 7 program,
8 instance watch (`CapType` in `kernel/include/kickos/cap.h`). `CapEntry::rights` is a bit set:
1 `WAIT` (receive, or wait), 2 `SIGNAL` (send, or signal), 4 `TRANSFER` (may be granted onward),
8 `HANDOUT` (an endpoint only, see Stop 8).

### Stop 1: a capability is a 16-byte entry
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
        // The whole generation of the object `obj` names, at the install; a pool handle
        // carries its low 16 bits. Read through cap_obj_live or cap_obj. A CAP_WATCH, whose
        // reference pins its object, holds its two list links here instead, armed or raised.
        uint32_t obj_gen;
        uint8_t type : KCAP_TYPE_BITS;
        // CAP_REPLY's call sequence, low half. cap_install_at zeroes both halves, so no entry
        // inherits the slot's last occupant.
        uint8_t seq_lo : KCAP_REPLY_SEQ_LO_BITS;
        uint8_t rights : KCAP_RIGHTS_BITS;
        uint8_t seq_hi : KCAP_REPLY_SEQ_HI_BITS; // the same field's high half
        uint16_t gen;                            // per-slot cap generation
        // CAP_ENDPOINT's badge, or CAP_NOTIFY's and CAP_WATCH's as bit + 1; KCAP_BADGE_NONE
        // unbadged. Seated by cap_install_at and never inherited.
        uint32_t badge;
    };
```

Read the fields one by one:

- `obj` names the object. It is not a pointer either: it is the object's handle in a global
  pool (one pool of endpoints, one of notifications, and so on). That handle carries a second
  generation, the object's own, so a slot that still names an object freed by someone else
  stops resolving. `obj_gen` keeps the whole of that generation, of which the handle holds only
  the low 16 bits.
- `type` says which pool `obj` belongs to.
- `rights` are what the holder may do. A notification wait needs `WAIT`, a send needs `SIGNAL`.
- `gen` is the slot's own generation. It goes up by one each time the slot is emptied.
- `badge` says who holds this copy: an endpoint's badge reaches the server with every message,
  and a notification copy's badge is the bit its holder raises.
- `seq_lo`/`seq_hi` hold a reply capability's call sequence.

The handle user code holds is `(gen << 16) | index` (`KCAP_INDEX_BITS` is 16). A fresh slot
has generation 0, so on a fresh table the handle equals the index. That is why `sink`'s
driver can write `KOS_SPAWN_DELEGATED_CAP0` (the number 1) and mean "my slot 1".

One slot is reserved in every table (`system/include/kickos/sys/cap_index.h`): slot 0,
`KOS_CAP_STDOUT`, the send-only console endpoint. Only the kernel fills it. Every other slot,
from `KOS_CAP_FIRST_DYNAMIC` (1) up, is the thread's own. A mutex is not in the table at all:
it is a word in the memory of the threads that share it, the C library's locks included
([Mutex](04-mutex.md)).

There is one more kind of power a thread can hold, and it is not in the table: the
**authority word**, `Thread::authority`. It is a bit set of what the thread may ask the
kernel to do that names no object: allocate memory (`KOS_AUTH_MEMORY`), create a task
(`KOS_AUTH_TASKS`), create a console driver (`KOS_AUTH_CONSOLE`), shut down
(`KOS_AUTH_SYSTEM`), and six more. `cap_check_authority` tests it.

**Try it:** this needs no running target; gdb reads the layout from the ELF. Stops 2 and 3
show real entries.

```
(gdb) p sizeof(kickos::CapEntry)
$1 = 16
(gdb) ptype kickos::CapEntry
type = struct kickos::CapEntry {
    int32_t obj;
    uint32_t obj_gen;
    uint8_t type : 4;
    uint8_t seq_lo : 4;
    uint8_t rights : 4;
    uint8_t seq_hi : 4;
    uint16_t gen;
    uint32_t badge;
}
```

**Why, and the contract:** [Book: Authority comes from possession, or from identity](../book/authority-from-possession-or-from-identity.md),
[Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)

### Stop 2: the table is a run of chunks from one slab
**Where:** `kernel/syscall/cap.cc`, `cap_slab_init`, `cap_slab_attach`, `cap_directory`;
`kernel/include/kickos/cap.h`, `CapChunkList`, `cap_slot`, `cap_run_free_build`

Every capability entry in the system lives in one array, the **slab**. The composition sizes
it, and `kickos_kernel_pools` names it (`cap_chunks`, `cap_chunk_count`). At boot
`cap_slab_init` cuts it into fixed-size **chunks** of `KCAP_CHUNK_SLOTS` entries (8) and
threads a free list through the free chunks themselves (`CapChunkList`): the first word of a
free chunk points to the next one.

A thread's table is a **run**: a few chunks taken from that list, every one or none
(`CapChunkList::take`). `Thread::caps` points to the thread slot's **directory** row
(`cap_directory`), an array of chunk pointers, and `cap_slot` turns an index into an entry with
a shift and a mask. `cap_slab_attach` reserves a run, zeroes it (a zeroed entry is an empty
slot with generation 0) and calls `cap_run_free_build`, which links every dynamic slot, from
index 1 up to the table's width, into a circular free list. The rest of the last chunk stays
out of the list.

That free list costs no memory of its own. A dead entry has no object to name, so its `obj`
word holds the list links: the next free index in the low half, the previous one in the high
half, both biased by one so that 0 can mean "none". `Thread::cap_free_head` is the head,
biased the same way.

Two widths matter. Root, the first thread, gets the init's width (`cap_root_width`, 8 here,
one chunk). Every other thread gets its task's declared width (16 here, two chunks) unless its
spawn asks for another (`spawn_cap_width`). Each task's `CAP_SLOTS` budget is charged the whole
chunks a spawn takes (`kcap_width_for`), and a spawn over that budget answers `-KOS_EAGAIN`.

The table never grows after the spawn. `cap_install` takes the head of the free list or
refuses with `-KOS_EMFILE`. That matters for servers: a client's call mints a reply
capability into the server's table, and a client must not be able to make the kernel
allocate on a server's behalf.

**Try it:** stop where the scheduler starts, before any thread has run, and look at root:

```
(gdb) break kickos::sched::start
(gdb) continue
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) set $P = &kickos::kickos_kernel_pools
(gdb) set $r = &$P->threads[0]
(gdb) p $r->caps[0]
$1 = (kickos::CapEntry *) 0x200022e0 <kickos::(anonymous namespace)::system_pools+6048>
(gdb) p $r->caps[1]
$2 = (kickos::CapEntry *) 0x0 <g_isr_vector>
(gdb) p $r->cap_width
$3 = 8
(gdb) p $r->cap_free_head
$4 = 2
(gdb) p $r->caps[0][1]
$5 = {obj = 524291, obj_gen = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0, badge = 0}
```

One chunk, and no second one. The free head is 2, which is index 1 biased by one: slot 1 is the
first free slot. Slot 1's `obj` is 524291, which is `0x00080003`: the previous free index is
`8 - 1 = 7` (the last slot, since the list is circular) and the next is `3 - 1 = 2`.

**Why, and the contract:** [Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
(the per-thread table, the chunk sizing, and the `KickOS: cap table =` configure line)

### Stop 3: what root holds when it starts
**Where:** `kernel/init/kmain.cc`, `kmain` (root's creation); `kernel/syscall/cap.cc`,
`cap_seat_authority`

Root is the first thread. [Power-on](01-power-on.md) followed `kmain` building it; here we
look only at what it is given. Root is unprivileged from its first instruction, like every
thread but idle. Its power is entirely in its table and its authority word:

```
        // Ordering matters twice: after thread_create, which zeroes the TCB and would wipe the
        // word; before sched::start(), so it is in place before root's first instruction.
        {
            IrqLock lock;
            cap_seat_authority(root_tcb, CAP_AUTH_ALL);
            task_run_join(root_tcb->task, root_slot);
        }
```

So root holds every authority bit (`CAP_AUTH_ALL`) and no capability at all. Slot 0 stays
empty: no console has been published, and a thread whose slot 0 is empty prints through the
kernel's own debug console instead. Everything else in the system is created later, by root or
by the threads it starts.

Root never runs the application directly. Its entry, `kickos_root_entry`
(`user/src/root_entry.cc`), runs the global constructors of the init's own image and then the
**init**, `kickos_init_entry` (`system/init/compose/entry.cc`), which walks the composition table.
Each task the init starts runs its own image's constructors at its entry
(`kickos_image_start_once`, `user/src/image_start.cc`).

**Try it:** in the same session, still at the `kickos::sched::start` stop of Stop 2:

```
(gdb) p $r->name
$6 = "root", '\000' <repeats 11 times>
(gdb) p/x $r->authority
$7 = 0x3ff
(gdb) p $r->privileged
$8 = false
(gdb) p *$r->caps[0]@2
$9 = {{obj = 0, obj_gen = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0, badge = 0},
      {obj = 524291, obj_gen = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0, badge = 0}}
```

The authority word has all ten bits. Slot 0 is empty (type 0), and so is slot 1, whose `obj`
is a free-list link (Stop 2).

**Why, and the contract:** [Book: Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md),
[Reference: invariants, `root-unprivileged-idle-alone-privileged`](../reference/invariants.md)

### Stop 4: the init hands authority down
**Where:** `system/init/compose/walk.cc`, `Walk::boot`, `Walk::start`, `Walk::watch`, `Walk::create`,
`Walk::release`; `kernel/syscall/syscall_thread.cc`, `task_create_call`;
`user/src/program_entry.cc`, `kickos_program_entry`

The init runs as root, unprivileged, and uses only system calls. `Walk::boot` sets up what
the init itself keeps for the life of the system: it lowers its own priority, creates a
notification object and binds itself to it (this is how it hears about deaths, see
[Fault and kill](09-fault-and-kill.md)), finds its program (`kos_program_find`), reserves a
private block of RAM for its records, and, for every task that serves an endpoint and does not
start on request, creates that endpoint and narrows its own copy to
`SIGNAL | TRANSFER | HANDOUT` with `kos_cap_narrow`. The init never receives on a server's
endpoint, so it gives up `WAIT` at once. `sink` starts on request, so its endpoint is the one
`main` creates.

Then `Walk::start` starts each task the table starts at boot. For `initdemo` there is one,
`main`:

```
            s.step = "kos_task_create";
            rc = take_lines(s);
            if (rc == 0)
            {
                rc = mint_uses(s);
            }
            if (rc == 0)
            {
                s.step = "kos_task_create";
                rc = create(s);
                handle = s.handle;
            }
            if (rc == 0)
            {
                // The task runs already: a watch armed after its end or its readiness reports it,
                // and a refused one would leave its death unreported.
                check(watch(i, handle, copy), "kos_watch", i);
            }
            release(s);
```

In words: take the task's interrupt lines and mint the badged copies it is granted; ask the
kernel to build the task and start it (`create`); then ask the kernel to raise a bit in the
init's notification when that task ends or is dead, and, for a server, when it first waits on
its endpoint. `Walk::watch` is one call, `kos_watch(KOS_WATCH_TASK, events, handle, copy, 0,
nullptr)`, and `copy` is a badged copy of the notification, minted by `kos_notify_badge` just
above. `Walk::create` fills a `kos_task_params` from the table entry and calls
`kos_task_create`:

```
        kos_task_params p = {};
        p.instance = name(s.task);
        p.cap_count = static_cast<uint8_t>(t.cap_grant_count);
        // ...
        int rc = kos_task_create(program, &p, &s.handle);
        if (rc == -KOS_EAGAIN)
        {
            s.step = "kos_watch";
            rc = park(s.task, program, s.copy);
            if (rc == 0)
            {
                s.step = "kos_task_create";
                rc = kos_task_create(program, &p, &s.handle);
            }
        }
```

`program` is the program capability that `Walk::boot` got from `kos_program_find`. The
declaration the kernel selects with `p.instance` is the static facts the composer wrote into
the image beside the table: the priority, the priority ceiling (12, from `ceiling:` in the
YAML), the cores and the authority. The init does not pass them. The kernel takes them from the
declaration, and it refuses a declaration whose authority bits, ceiling or cores the creator
does not hold. The kernel then starts the task's entry thread itself. For a program the entry is
`kickos_program_entry`, which calls the program's `main`. A create never waits: an instance
whose last incarnation is still dying answers `-KOS_EAGAIN`, and `Walk::park` then arms a
`KOS_WATCH_INSTANCE` watch that says when it can start again (Stop 6).
Finally `Walk::release` closes the badged copy: the kernel's watch keeps its own record, so the
init does not need the capability any more.

**Try it:** `break kickos::task_create_call`, `continue`. The first hit is the init creating
`main` (the backtrace goes through `syscall_dispatch` from `svc_trampoline`), and
`p *(kos_task_params*)pu` shows `instance = ... "main"`. Now `break kickos::handle_close`,
`continue`: the init closes handle 3, its badged copy.

```
(gdb) p c->caps[0][1]
$2 = {obj = 0, obj_gen = 0, type = 6 '\006', seq_lo = 0 '\000', rights = 7 '\a', seq_hi = 0 '\000', gen = 0, badge = 0}
(gdb) p c->caps[0][2]
$3 = {obj = 1, obj_gen = 0, type = 7 '\a', seq_lo = 0 '\000', rights = 4 '\004', seq_hi = 0 '\000', gen = 0, badge = 0}
(gdb) p c->caps[0][3]
$4 = {obj = 0, obj_gen = 0, type = 6 '\006', seq_lo = 0 '\000', rights = 7 '\a', seq_hi = 0 '\000', gen = 0, badge = 1}
(gdb) p/x $P->tasks[3]
$5 = {domain = 0x20003294, free_node = {next = 0x0, prev = 0x0}, members = {head = 0x20002b28}, refcount = 0x1, creator_tag = 0x1, gen = 0x0, prio_ceiling = 0xc, marks = 0x80, watch_bit = 0xc0, sweeping = 0x0, image = 0x1, watch_notify = 0x1, watch_ep = 0x0, exit_status = 0x82, reent_seat = 0x20010424}
(gdb) p $P->threads[1].name
$6 = "main", '\000' <repeats 11 times>
(gdb) p/x $P->threads[1].authority
$7 = 0x71
```

Slot 1 is the init's notification (type 6). Slot 2 is the program capability (type 7). Slot 3
is the badged copy of the notification: its badge is stored as bit + 1, so `badge = 1` means
bit 0. A declared task's record is fixed by its declaration, counted from the end of the task
array (`decl_slot` in `kernel/task/task.cc`), so `main`, the first declaration, is in
`tasks[3]`. That task has one member (`main`), was created by the thread whose kill tag is 1
(root, slot 0), has its ceiling, is marked started (`0x80`), and carries the watch:
`watch_bit` holds bit 0 with the events it asks for above it. `exit_status` starts at 130
(`KOS_EXIT_CANCELLED`): a task whose members are all cancelled ends with that status. Root
holds `0x3ff`; `main` holds `0x71`, the four bits its composition lists: memory (1), system
(16), console (32) and tasks (64).

**Why, and the contract:** [Reference: invariants, "App entry & init seam"](../reference/invariants.md#app-entry--init-seam),
[Book: Authority comes from possession, or from identity](../book/authority-from-possession-or-from-identity.md)

### Stop 5: an own-create takes the head of the free list
**Where:** `kernel/syscall/syscall_ipc.cc`, `endpoint_create`; `kernel/sync/sync.cc`,
`endpoint_claim_created`; `kernel/syscall/cap.cc`, `cap_install`, `cap_install_at`

`main`'s first call after its `printf` is `kos_endpoint_create`. The syscall arm
`KOS_SYS_ENDPOINT_CREATE` checks the out-pointer (`cap_out_check`), then calls
`endpoint_create`:

```
        Endpoint* ep = nullptr;
        int const i = endpoint_claim_created(c->task, &ep);
        if (i == SLOT_OVER_BUDGET)
        {
            return -KOS_EAGAIN; // the task holds its budget of endpoints
        }
        // The bound, not the sign: an index this unit cannot bound reads as out of range below.
        if (kernel().endpoints.at(i) == nullptr)
        {
            return -KOS_ENOMEM;
        }
        kernel().endpoints.refs()[i] = 1;
        int const obj = kernel().endpoints.handle_for(i);
        int const rc = cap_install(c, obj, CapType::CAP_ENDPOINT, CAP_RIGHTS_ALL, out_cap);
```

`endpoint_claim_created` takes a free pool slot and charges it to the caller's task. A task
that already holds its budget of endpoints is refused with `-KOS_EAGAIN`, so one task cannot
empty a pool that others need; a full pool is `-KOS_ENOMEM`. `refs()[i] = 1` is the creator's
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
(gdb) p $k->currents.cells[0]->cap_width
$1 = 16
(gdb) break kickos::cap_install
(gdb) continue
(gdb) p c->name
$2 = "main", '\000' <repeats 11 times>
(gdb) p c->cap_free_head
$3 = 2
(gdb) finish
Value returned is $4 = 0
(gdb) p/x *out_cap
$5 = 0x1
(gdb) p $k->currents.cells[0]->caps[0][1]
$6 = {obj = 0, obj_gen = 0, type = 1 '\001', seq_lo = 0 '\000', rights = 15 '\017', seq_hi = 0 '\000', gen = 0, badge = 0}
(gdb) p $k->currents.cells[0]->cap_free_head
$7 = 3
(gdb) p/d $P->endpoints.arrays.refs[0]
$8 = 1
```

`main`'s table is 16 wide. It gets handle 1: slot 1, generation 0. The entry names endpoint 0
(pool index 0, generation 0) with all four rights (15). The free head went from 2 to 3. The
head is the slot number plus one, so it named slot 1 before the call and names slot 2 after it.

**Why, and the contract:** [Book: Adding a kernel object type](../book/adding-a-kernel-object-type-the-additive-recipe.md),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
("Lifecycle", and the own-create rule)

### Stop 6: a task is a declared instance with a creator
**Where:** `kernel/syscall/syscall_thread.cc`, `task_create_call`, `task_create_locked`,
`program_instance`, `decl_admit`, `task_build`; `kernel/task/task.cc`, `task_decl_state`,
`task_decl_in_the_way`, `task_create_declared`, `task_handle`

A **task** is a group of threads that share one memory domain and live and die together: a
fault in one ends the whole task ([Fault and kill](09-fault-and-kill.md)). A task is always an
instance of a declaration the composer wrote into an image. `main` finds a program capability
with `kos_program_find` (it lands in slot 2) and asks for the instance named `sink`, so that
`sink` can die without taking `main` along.

`task_create_locked` runs the whole create under one `IrqLock`. In order:

1. **Copy the parameters in** (`task_params_in`), and pick the declaration
   (`program_instance`, which resolves the program capability and needs `AUTH_TASKS`).
2. **May this caller start it?** `decl_admit` refuses a declaration that does not start on
   request unless the caller is root, and every authority bit the declaration needs that the
   caller lacks: its own bits, and `AUTH_CONSOLE` for a console driver like `sink`. It also
   refuses a ceiling above the caller's.
3. **Is the instance free?** `task_decl_state` asks `task_decl_in_the_way`:

```
        Task const& t = decl_slot(decl);
        bool const occupied = t.refcount != 0 or t.sweeping != 0;
        if (occupied and task_ended(&t))
        {
            return -KOS_EAGAIN;
        }
        if (occupied)
        {
            return -KOS_EBUSY;
        }
```

   The create never waits. A live instance answers `-KOS_EBUSY`. An instance whose last
   incarnation has ended but is not yet dead, its members still running to their deaths or
   sweeping their capabilities, answers `-KOS_EAGAIN`, and so does one whose declared line or
   IRQ binding is still retiring. A `KOS_WATCH_INSTANCE` watch reports when nothing is in the
   way any more.

4. **Stage the grants** (`create_grants_in`), and find the declaration's ready endpoint among
   them (`ready_endpoint`). For a packaged driver, `driver_admit` stages its device windows and
   admits its lines.
5. **Build the task** (`task_build`). `task_create_admit` checks `AUTH_TASKS` again and any
   memory grant. `task_create_declared` takes the declaration's own task record, ends the dead
   incarnation's creator hold, picks the memory domain (`domain_for`), and records the creator.
   `main` passes no memory, so on this region-MPU board the task shares the default user domain
   (`domain_default_user`); on a board with page tables it would get an address space of its
   own ([The MMU](10-the-mmu.md)). Back in `task_build`, `task_sched_inherit` copies the
   creator's scheduling grant and the declaration narrows it to its own ceiling and cores.
6. **Spawn the entry thread** with `spawn_locked` (Stops 8 to 10), at the declared priority and
   authority, with the grants staged above.
7. **Publish the console** for a declaration that serves it (Stop 12), claim a driver's lines,
   and write the task's handle (`task_handle`) to the caller.

`creator_tag` records who created the task: only that thread may watch it or end it
(`task_created_by`). A thread's tag is its pool index plus one. `task_handle` packs the slot
index (plus one) and the generation into the `kos_task_t` handle.

A task is not a capability. It is named by its own generational handle, and the authority to
use that name is "I created it", checked by tag.

**Try it:** the second `task_create_call` is `main`'s.

```
(gdb) break kickos::task_create_call if $_streq($k->currents.cells[0]->name, "main")
(gdb) continue
(gdb) p *(kos_task_params*)pu
$1 = {argv = 0x0 <g_isr_vector>, block = 0x0 <g_isr_vector>, size = 0, flags = 0, instance = 0x1603d "sink", caps = 0x20025f54, cap_dest = 0x0 <g_isr_vector>, windows = 0x0 <g_isr_vector>, stack = 0x0 <g_isr_vector>, stack_size = 0, window_count = 0, cap_count = 1 '\001', rsv0 = 0 '\000'}
(gdb) p *((kos_task_params*)pu)->caps
$2 = {source_cap = 1, rights_mask = 5 '\005'}
(gdb) set $o = (unsigned*)out
(gdb) finish
Value returned is $3 = 0
(gdb) p/x *$o
$4 = 0x3
(gdb) p/x $P->tasks[2]
$5 = {domain = 0x20003294, free_node = {next = 0x0, prev = 0x0}, members = {head = 0x20002ca0}, refcount = 0x1, creator_tag = 0x2, gen = 0x0, prio_ceiling = 0xc, marks = 0x90, watch_bit = 0x0, sweeping = 0x0, image = 0x2, watch_notify = 0x0, watch_ep = 0x1, exit_status = 0x82, reent_seat = 0x20018000}
```

`finish` lands in `syscall_body`. The handle is 3, which is slot 2: `sink`, the second
declaration. The task has one member, its creator tag is 2 (`main` is pool slot 1), its domain
is the same one `main`'s task uses, and it is marked started and serving the console
(`0x90`). `watch_ep` names its ready endpoint, the one `main` granted.

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

- **The caller brings one.** A caller calls `kos_ram_alloc` and passes the block to the spawn
  or the create. The `KOS_SYS_RAM_ALLOC` arm checks `AUTH_MEMORY`, then `ram_owner_alloc`
  takes the block and records which task reserved it (`kernel/include/kickos/ramown.h`): a
  later grant of that block is admitted only to its owner. The block is cleared before it is
  returned.
- **The kernel picks one.** A spawn with `stack_base == nullptr` gets a block of
  `KICKOS_USER_STACK_SIZE` (8 KiB here). `spawn_locked` first tries the free list
  (`ThreadPool::stack_pop`, zeroing a recycled block), then `arch_ram_alloc`. Such a stack is
  marked `kstack_owned`, and when the thread's pool slot is reused (`ThreadPool::alloc`) the
  block goes back on that free list. One size class, so the list never fragments.

**Try it:** `break arch_ram_alloc` and `break kickos::ThreadPool::stack_pop`, then `continue`
nine times with `bt 2` at each stop. Trimmed to the caller frame:

```
Breakpoint 1, arch_ram_alloc (size=512)
#1  kickos::(anonymous namespace)::boot_stack_alloc (size=size@entry=512, exhausted_msg=... "kmain: no arena for the idle stack")
Breakpoint 1, arch_ram_alloc (size=8192)
#1  kickos::(anonymous namespace)::boot_stack_alloc (size=size@entry=8192, exhausted_msg=... "kmain: no arena for the root stack")
Breakpoint 1, arch_ram_alloc (size=128)
#1  kickos::ram_owner_alloc (owner=0x2000341c <...>, size=size@entry=128)
Breakpoint 2, kickos::ThreadPool::stack_pop (this=<optimized out>)
#1  kickos::spawn_locked (p=..., kname=... "main", ...)
Breakpoint 1, arch_ram_alloc (size=8192)
#1  kickos::spawn_locked (p=..., kname=... "main", ...)
Breakpoint 2, kickos::ThreadPool::stack_pop (this=<optimized out>)
#1  kickos::spawn_locked (p=..., kname=... "sink", ...)
Breakpoint 1, arch_ram_alloc (size=8192)
#1  kickos::spawn_locked (p=..., kname=... "sink", ...)
Breakpoint 2, kickos::ThreadPool::stack_pop (this=<optimized out>)
#1  kickos::spawn_locked (p=..., kname=... "worker", ...)
Breakpoint 1, arch_ram_alloc (size=8192)
#1  kickos::spawn_locked (p=..., kname=... "worker", ...)
```

In order: idle's stack, root's stack, the init's private block (128 bytes), then `main`'s,
`sink`'s and `worker`'s stacks, each picked by the kernel. On this build the thread pointer is
found by masking SP, so every stack is one stride the kernel hands out, and the init leaves
every stack to the kernel (`Build::sp_masked` in `system/init/compose/walk.h`). Before each
kernel-picked stack, `stack_pop` finds the free list empty, so `arch_ram_alloc` follows:
nobody has died yet.

**Why, and the contract:** [Book: Thread stacks](../book/thread-stacks-and-the-kiss-tension.md),
[Reference: architecture, "Memory domains (the isolation unit)"](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 8: the spawn checks everything before it claims anything
**Where:** `kernel/syscall/syscall_thread.cc`, `spawn_locked`, `spawn_admit`, `spawn_stack_admit`,
`spawn_windows_admit`, `spawn_authority_admit`, `spawn_masked`, `spawn_params_in`

Every thread is made by `spawn_locked`. A task create calls it for the entry thread, with the
grants it already staged. `kos_thread_create` reaches it through `thread_create_call` and
`spawn_masked`, which first copies the `kos_thread_params` into a kernel copy
(`spawn_params_in`) and the name byte by byte, so the caller cannot change a field after it
was checked. The whole spawn runs under one `IrqLock`. Its first half only reads and checks.
Nothing is claimed until every check that can be done up front has passed, so a refused spawn
leaves nothing behind.

1. **`spawn_admit`.** The priority is in range (priority 0 is idle's alone). The stack, if
   given, is large enough, aligned, and (with the MPU enforcing) a block the caller may grant
   (`spawn_stack_admit`). The device windows are admissible (`spawn_windows_admit`). Every
   authority bit asked for is one the caller holds (`spawn_authority_admit`): authority only
   narrows on the way down.
2. **The grant list.** Copied in one pass, then checked entry by entry:

```
            for (int ci = 0; ci < ncaps; ci++)
            {
                kos_cap_t const source = deleg_badge[ci];
                uint8_t const mask = deleg_kind[ci];
                CapEntry* se = cap_lookup(spawner, source);
                if (se == nullptr or not cap_obj_live(*se))
                {
                    return -KOS_EBADF;
                }
                if ((se->rights & CAP_TRANSFER) != CAP_TRANSFER)
                {
                    return -KOS_EACCES;
                }
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
   slot, and two grants may not land on the same slot (`-KOS_EINVAL`).

3. **The task.** `kos_thread_create` adds the child to the caller's own task; a create adds the
   entry thread to the task it just built. An ended task takes no member (`-KOS_EBUSY`), and
   the priority must not exceed the task's ceiling (`-KOS_EPERM`).

Only then does it claim: the task's `CAP_SLOTS` budget for the child's width, a thread slot
(`ThreadPool::alloc`, charged to the task), a stack (Stop 7), the argument block at the top of
the stack for a create's entry thread, the MPU region set (`thread_regions_stage`), a
capability run of the child's width (`cap_slab_attach`), a check of the grants against the
destination task's object budgets (`task_object_admit_grants`), and last, one reference per
granted object (`obj_ref_inc`). Each failure undoes what was claimed before it
(`spawn_unwind`). The reference bump is last because it is the one step whose undo would
otherwise have to reach into the finished child.

**Try it:** stop on the TCB build for `sink`, then return into `spawn_locked` to see what was
staged:

```
(gdb) break kickos::thread_create if $_streq(attr.name, "sink")
(gdb) continue
(gdb) p attr.cap_width
$1 = 16
(gdb) p attr.task->refcount
$2 = 0 '\000'
(gdb) p stack_base
$3 = (void *) 0x20026000
(gdb) p stack_size
$4 = 8192
(gdb) p arg
$5 = (void *) 0x20027fe0
(gdb) finish
kickos::spawn_locked (...) at .../kernel/syscall/syscall_thread.cc:1292
1292	        task_run_join(tk, i);
(gdb) p deleg_dest[0]
$6 = 1
(gdb) p/x deleg_kind[0]
$7 = 0x15
(gdb) p/x deleg_obj[0]
$8 = 0x0
(gdb) p/d $P->endpoints.arrays.refs[0]
$9 = 2
```

The child's table is 16 wide. The task has no member yet. The stack is the block the kernel
picked in Stop 7, and the argument is the `kos_task_args` block at its top. The single grant
goes to child slot 1. `deleg_kind` packs the kind and the rights in one byte: `0x1` endpoint,
`0x5` `WAIT | TRANSFER`. It names endpoint 0, which now has two references: `main`'s and the
one taken for `sink`.

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md)
(validate, then use), [Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
(the B1 wire contract and delegation), [Reference: invariants, "Syscall, privilege & the user/kernel boundary"](../reference/invariants.md#syscall-privilege--the-userkernel-boundary)

### Stop 9: thread_create builds the TCB and a first frame
**Where:** `kernel/thread/thread.cc`, `thread_create`, `tls_carve`;
`kernel/syscall/syscall_thread.cc`, `thread_regions_stage`;
`arch/arm/armv7m/arch_armv7m.cc`, `arch_context_init`

`thread_create` fills the thread control block (TCB) from the staged `ThreadAttr`. It zeroes
the TCB first, then seats the capability run and its free head (after the zeroing, or they
would be wiped), copies the name, sets the priority and policy, and takes a reference on the
task (`task_ref`): a thread holds its task for its whole life, and `task_ref` takes the
domain reference when the group goes from empty to one member.

On this board the MPU region set was staged by `spawn_locked` (`thread_regions_stage`) and is
copied in whole: the code every unprivileged thread may run, the static data of the task's own
image, the thread's own device windows, and its own stack. The kernel then carves the thread's
C library state and thread-local storage off the low end of the stack and seats the thread's
private kernel stack (`ctx.kernel_sp`, one block per pool slot).

Last, `arch_context_init` fabricates the frame that the first context switch will "return"
into: exactly what the hardware would have stacked on an exception, plus the registers
`PendSV_Handler` saves. [01, Stop 8](01-power-on.md#stop-8-idle-and-root-are-fabricated-as-if-they-had-already-been-switched-out)
walks that code for root; `sink` gets the same frame. The return address is the thread-return
stub of the thread's own image (`kickos_image_thread_return` holds its address): a thread whose
function returns falls into the exit path. `EXC_RETURN` is kept in `ctx.exc_return`, not on the
stack, and `npriv = 1` makes the thread unprivileged.

**Try it:**

```
(gdb) break kickos::thread_create if $_streq(attr.name, "sink")
(gdb) continue
(gdb) set $t = t
(gdb) finish
(gdb) p/x $t->ctx
$1 = {sp = 0x20027fa0, npriv = 0x1, resting_npriv = 0x1, stack_lo = 0x20026180, stack_hi = 0x20027fe0, kernel_sp = 0x20001710, exc_return = 0xfffffffd}
(gdb) x/16wx $t->ctx.sp
0x20027fa0:	0x00000000	0x00000000	0x00000000	0x00000000
0x20027fb0:	0x00000000	0x00000000	0x00000000	0x00000000
0x20027fc0:	0x20027fe0	0x00000000	0x00000000	0x00000000
0x20027fd0:	0x00000000	0x00017261	0x000173c4	0x01000000
(gdb) info symbol 0x17260
kickos_user_thread_return in section .kickos_image2_text
(gdb) info symbol 0x173c4
kickos_task_entry in section .kickos_image2_text
(gdb) p $t->mpu.regions_
$2 = {{base = 0, size = 4194304, attr = 5}, {base = 536969216, size = 32768, attr = 3}, {base = 537026560, size = 8192, attr = 3}, {base = 0, size = 0, attr = 0}, ...}
```

Read the frame from the bottom up: eight zeros for r4-r11, r0 = `0x20027fe0` (the argument
block), three zeros and r12, LR = `0x17261` (the image's `kickos_user_thread_return`, Thumb
bit set), PC = `0x173c4` (`kickos_task_entry`, the entry of a packaged driver's task), and xPSR
with only the Thumb bit. The usable stack starts at `0x20026180`, above the carve, and ends at
`0x20027fe0`, below the argument block. The MPU set has three regions: flash, read and execute
(`0x0`, 4 MiB); the static data of `sink`'s image, read and write (`0x20018000`, 32 KiB); and
the stack, read and write (`0x20026000`, 8 KiB). [Shared memory](06-shared-memory.md) follows
how those are committed at the context switch.

**Why, and the contract:** [Book: Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md),
[Reference: invariants, "Context switch & FP save/restore"](../reference/invariants.md#context-switch--fp-saverestore)

### Stop 10: seat the slots, then make it runnable
**Where:** `kernel/syscall/syscall_thread.cc`, the end of `spawn_locked`, `task_create_locked`,
`task_create_call`; `kernel/syscall/cap.cc`, `cap_install_defaults`, `cap_seat_authority`,
`cap_install_at`; `kernel/sched/sched.cc`, `sched::add`

After `thread_create` nothing may fail. The order is:

```
        thread_create(&k.threads.slots()[i], p->entry, arg, stack, stack_size, attr);
        // Nothing below here may fail: the validation above guarantees each install succeeds,
        // and the reference loop already holds a reference for every cap seated here.
        Thread* const made = &k.threads.slots()[i];
        task_run_join(tk, i);
        cap_install_defaults(made, lock);
        cap_seat_authority(made, p->authority);
        for (int ci = 0; ci < ncaps; ci++)
        {
            cap_install_at(made, static_cast<int>(deleg_dest[ci]), deleg_obj[ci],
                           static_cast<CapType>(kcap_grant_type(deleg_kind[ci])),
                           kcap_grant_rights(deleg_kind[ci]), deleg_badge[ci]);
        }
        // ...
        if (into == nullptr)
        {
            sched::add(made, lock);
        }
```

`cap_install_defaults` fills slot 0 with a send-only copy of the published console endpoint,
if one is published, and leaves it empty otherwise. The authority word is seated from the
(already checked) request; for an entry thread that is the declaration's authority. Each grant
goes to its slot, with the badge it had in the parent.

`sched::add` marks the child READY and links it into a ready queue. A thread of the caller's
own task is added here. A create's entry thread is added by the create once nothing can fail,
and a console driver's only after the lock is dropped and the kernel's last console writers
have finished (`task_create_call`). Creation then follows the rule written above
`kos::thread::create` in `user/include/kickos/kos.h`: a child that outranks its creator runs
before the call returns, and one that does not runs once the creator blocks or yields. `sink`
(priority 12) outranks `main` (priority 2), so `sink` runs before `main`'s `kos_task_create`
returns.

**Try it:** watch the order of events. Run to `main`'s first call so the init's own spawns and
closes are behind you, then set five breakpoints and print the running thread's name at each
stop:

```
(gdb) break kickos::endpoint_create
(gdb) continue
(gdb) delete
(gdb) break kickos::sched::add
(gdb) break kickos::cap_console_publish_through
(gdb) break kickos::handle_close
(gdb) break kickos::cap_narrow
(gdb) break kickos::cap_resolve_e if $_streq(c->name, "sink")
```

Then seven times: `continue`, `p $k->currents.cells[0]->name`. Trimmed:

```
Breakpoint 4, kickos::cap_console_publish_through (publisher=publisher@entry=0x200029d8 <...>, ...)
$1 = "main", '\000' <repeats 11 times>
Breakpoint 3, kickos::sched::add (t=t@entry=0x20002b50 <...>, held=held@entry=...)
$2 = "main", '\000' <repeats 11 times>
Breakpoint 6, kickos::cap_narrow (c=0x20002b50 <...>, cap_handle=cap_handle@entry=1, mask=mask@entry=1, ...)
$3 = "sink", '\000' <repeats 11 times>
Breakpoint 7, kickos::cap_resolve_e (c=c@entry=0x20002b50 <...>, cap_handle=cap_handle@entry=1, ...)
$4 = "sink", '\000' <repeats 11 times>
Breakpoint 5, kickos::handle_close (c=0x200029d8 <...>, cap_handle=cap_handle@entry=2, ...)
$5 = "main", '\000' <repeats 11 times>
Breakpoint 6, kickos::cap_narrow (c=0x200029d8 <...>, cap_handle=cap_handle@entry=1, mask=mask@entry=14, ...)
$6 = "main", '\000' <repeats 11 times>
Breakpoint 3, kickos::sched::add (t=t@entry=0x20002cc8 <...>, held=held@entry=...)
$7 = "main", '\000' <repeats 11 times>
```

The create publishes the console before `sink` (the TCB at `0x20002b50`) is added. `sink`
then runs at once: it narrows its grant and parks in its first receive. Only after that does
`main` close its program capability, narrow its endpoint and add `worker` (the TCB at
`0x20002cc8`).

**Why, and the contract:** [Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
(the B1 wire contract and the reserved slots), [Reference: architecture, "Scheduler (the core constraint)"](../reference/architecture.md#scheduler-the-core-constraint)

### Stop 11: the child uses its grant through the resolve
**Where:** `user/src/driver_service.cc`, `bring_up`; `kernel/syscall/syscall_ipc.cc`,
`endpoint_reply_recv`, `endpoint_recv_locked`; `kernel/syscall/cap.cc`, `cap_resolve_e`,
`cap_lookup`

`sink`'s entry thread runs the driver library's `bring_up`, which keeps on each grant only the
rights its descriptor names (here `WAIT`, with `kos_cap_narrow`) and then runs the service loop
of `appsink.cc`. That loop calls `kos_reply_recv` on handle 1 to wait for a message. The kernel
turns that handle into an endpoint through `cap_resolve_e`, the one function every syscall that
names an object goes through. [The first syscall, Stop 8](02-first-syscall.md) walks it line by
line. In short: `cap_lookup` checks that the index is inside this thread's table, that the slot
is not empty, and that the generation matches; `cap_resolve_e` then checks the type, then the
rights (`(rights & need) == need`), then that the object is the one the slot was given, then
asks the object pool, which checks the object's own generation. The caller holds `IrqLock` from
the resolve through the use, so no close can free the object in between.

The point to notice here is whose table is searched: `c`, the calling thread's own. The same
number 1 names `main`'s endpoint capability in `main`'s table and `sink`'s narrowed copy in
`sink`'s table. Possession is per thread.

**Try it:**

```
(gdb) break kickos::cap_resolve_e if $_streq(c->name, "sink")
(gdb) continue
(gdb) p want
$1 = kickos::CapType::CAP_ENDPOINT
(gdb) p need
$2 = 1 '\001'
(gdb) p c->caps[0][1]
$3 = {obj = 0, obj_gen = 0, type = 1 '\001', seq_lo = 0 '\000', rights = 1 '\001', seq_hi = 0 '\000', gen = 0, badge = 0}
(gdb) p/x c->authority
$4 = 0x0
(gdb) p c->caps[0][0]
$5 = {obj = 0, obj_gen = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 0, badge = 0}
(gdb) bt 4
#0  kickos::cap_resolve_e (c=c@entry=0x20002b50 <...>, cap_handle=cap_handle@entry=1, want=want@entry=kickos::CapType::CAP_ENDPOINT, need=need@entry=1 '\001', ...)
#1  0x00008034 in kickos::endpoint_recv_locked (ask=<synthetic pointer>..., c=0x20002b50 <...>, cap=1, ...)
#2  kickos::endpoint_reply_recv (reply_cap=reply_cap@entry=4294967295, ...)
#3  0x00006dd2 in (anonymous namespace)::syscall_body (nr=68, ...)
(gdb) finish
Value returned is $6 = (void *) 0x200034a0 <kickos::(anonymous namespace)::system_pools+10592>
```

A receive needs `WAIT` (1), and `sink`'s slot 1 carries exactly that: the create granted
`WAIT | TRANSFER`, and `bring_up` gave back `TRANSFER`. `sink` has no authority at all: its
declaration names none. Its slot 0 is empty: a thread of the task that serves the console gets
no stdout, or it would send its own prints to itself (Stop 12).

**Why, and the contract:** [Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
("One resolve chokepoint")

### Stop 12: publishing the console fills slot 0 for later children
**Where:** `kernel/syscall/syscall_thread.cc`, `driver_publish`; `kernel/syscall/cap.cc`,
`cap_console_publish_through`, `cap_console_publish`, `cap_seat_stdout`, `cap_console_serve`,
`cap_install_defaults`

`sink`'s declaration serves the console, so creating it needs `AUTH_CONSOLE`, and its create
publishes the console through the capability the declaration names as its ready endpoint:
`main`'s, granted in the create. `driver_publish` calls `cap_console_publish_through`. That
capability must carry `HANDOUT`. `cap_console_publish` then takes two references on the
endpoint: one for the kernel itself (the published target, so the endpoint outlives every
thread's copy), and one for the publisher's own slot 0, which `cap_seat_stdout` fills with a
`SIGNAL`-only copy, badged with the `write` right. A publisher's capability without `WAIT`
gains it here and counts as a receiver until the publisher narrows it; `main`'s already holds
`WAIT`. `cap_console_serve` records that `sink`'s task serves the console and empties slot 0 of
its members.

From now on, every spawn's `cap_install_defaults` seats slot 0 of the new child on this
endpoint. This is the "default capability set" a plain application gets without writing any
manifest: `worker` just calls `printf`, and the C library's `_write` sends on slot 0.

**Try it:**

```
(gdb) break kickos::cap_console_publish_through
(gdb) continue
(gdb) p e->rights
$1 = 15 '\017'
(gdb) p/d $P->endpoints.arrays.refs[0]
$2 = 2
(gdb) finish
Value returned is $3 = 0
(gdb) p/d $P->endpoints.arrays.refs[0]
$4 = 4
(gdb) p $k->currents.cells[0]->caps[0][0]
$5 = {obj = 0, obj_gen = 0, type = 1 '\001', seq_lo = 0 '\000', rights = 2 '\002', seq_hi = 0 '\000', gen = 0, badge = 16777216}
```

`main`'s slot 0 is now a send-only (`SIGNAL`, 2) copy of endpoint 0, and the endpoint has two
more references. The badge `0x01000000` is the `write` right (`KOS_RIGHT_WRITE`), which `sink`
checks before it counts a send. Later, at `worker`'s spawn:

```
(gdb) break kickos::cap_install_defaults
(gdb) continue
(gdb) continue
(gdb) continue
(gdb) p child->name
$6 = "worker\000\000\000\000\000\000\000\000\000"
(gdb) finish
(gdb) p $P->threads[3].caps[0][0]
$7 = {obj = 0, obj_gen = 0, type = 1 '\001', seq_lo = 0 '\000', rights = 2 '\002', seq_hi = 0 '\000', gen = 0, badge = 16777216}
(gdb) p/d $P->endpoints.arrays.refs[0]
$8 = 5
```

**Why, and the contract:** [Reference: console, "Capability handover"](../reference/console.md#capability-handover),
[Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
("Low-barrier is a hard constraint")

### Stop 13: closing a capability
**Where:** `kernel/syscall/cap.cc`, `handle_close`, `obj_close_protocol`, `endpoint_rights_dropped`,
`cap_slot_vacate`, `obj_ref_drop`

`main` no longer needs its program capability. `kos_handle_close(prog)` reaches
`handle_close`:

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
        obj_close_protocol(c, *e, /*teardown=*/false, held);
        CapEntry const detached = *e;
        // Vacated BEFORE the ref drops, so no stale handle resolves during the drop, and
        // after the copy above, whose obj the vacate overwrites.
        cap_slot_vacate(c, cap_handle & KCAP_INDEX_MASK, e);
        obj_ref_drop(detached, /*teardown=*/false, held);
        return 0;
```

Four steps:

1. **Look up**, with the same `cap_lookup` as a resolve. A reserved slot cannot be closed.
2. **The type's close protocol** (`obj_close_protocol`). Most types have nothing to do, a
   program among them, and none refuses the close. An endpoint counts its receivers and its
   `HANDOUT` holders, so `endpoint_rights_dropped` takes this capability's rights off those
   counts. If it was the last receiver, the endpoint is marked vacated and every parked sender
   is answered `-KOS_ECONNREFUSED` at once (`refuse_senders`) rather than left waiting, unless
   the endpoint is the console a task serves.
3. **Vacate the slot** (`cap_slot_vacate`): generation + 1, type empty, rights 0, and the slot
   goes to the **tail** of the free list. The tail, not the head, so a slot is reused as late
   as possible and its generation turns over slowly.
4. **Drop the reference** (`obj_ref_drop`): `refs - 1`, and the object is freed when it
   reaches 0. Destroy-on-last-close. A program is a static record and counts no references.

From the moment of step 3, handle 2 in `main` is stale. If `main` used it again,
`cap_lookup` would find generation 1 where the handle says 0, and answer `-KOS_EBADF`. When
slot 2 is reused, its new handle is `0x00010002`, not 2.

**Try it:**

```
(gdb) break kickos::handle_close if $_streq(c->name, "main")
(gdb) continue
(gdb) p cap_handle
$1 = 2
(gdb) p c->caps[0][2]
$2 = {obj = 1, obj_gen = 0, type = 7 '\a', seq_lo = 0 '\000', rights = 4 '\004', seq_hi = 0 '\000', gen = 0, badge = 0}
(gdb) p c->cap_free_head
$3 = 4
(gdb) finish
Value returned is $4 = 0
(gdb) p $k->currents.cells[0]->caps[0][2]
$5 = {obj = 1048580, obj_gen = 0, type = 0 '\000', seq_lo = 0 '\000', rights = 0 '\000', seq_hi = 0 '\000', gen = 1, badge = 0}
(gdb) p $k->currents.cells[0]->cap_free_head
$6 = 4
```

The program capability carries `TRANSFER` alone. After the close the slot's generation is 1,
its `obj` now holds free-list links (`1048580` is `0x00100004`: previous index 15, the last
slot, and next index 3), and the free head is unchanged at index 3, because the released slot
went to the tail.

**Why, and the contract:** [Book: Naming a kernel object, "Lifecycle"](../book/handles-and-the-resolve-chokepoint.md#lifecycle-refcount-and-destroy-on-last-close),
[Book: A stale handle must not be usable](../book/stale-handles-generation-width-and-allocation-policy.md),
[Book: Adding a kernel object type](../book/adding-a-kernel-object-type-the-additive-recipe.md)
(the close protocol before the reference drop)

### Stop 14: the other ways a capability changes hands
**Where:** `kernel/syscall/cap.cc`, `cap_narrow`, `cap_install_reply`, `cap_teardown`;
`kernel/notify/notify.cc`, `notify_badge`

The spawn's grant list, and the create's, are the paths by which a thread copies a capability
it holds into another thread's table. The other ways a table changes are narrower:

- **Narrowing in place.** `kos_cap_narrow(cap, mask)` (`cap_narrow`) keeps `rights & mask`.
  It never widens. Dropping `WAIT` or `HANDOUT` from an endpoint capability moves the
  endpoint's counts exactly as a close of those rights would. With the pseudo-handle
  `KOS_CAP_AUTHORITY` it narrows the authority word instead. The init narrows its server
  endpoints this way (Stop 4), `sink`'s driver library its grant (Stop 11), and `main` its
  endpoint, to `SIGNAL | TRANSFER | HANDOUT`, so `sink` is the only receiver.
- **A badged copy in your own table.** `kos_notify_badge` mints a second capability to the
  same notification with a badge, so the bit a holder raises says who raised it. The init uses
  one per task it starts. `kos_endpoint_badge` does the same for an endpoint.
- **A reply capability**, minted by the kernel into a server's table when a client calls
  (`cap_install_reply`). It names the waiting caller, not an object, and is used once.
  [Call and reply](05-call-and-reply.md) follows it.
- **Everything at once on exit.** `cap_teardown` closes every slot of a dying thread through
  the same protocol and reference drop as `handle_close`. [Fault and kill](09-fault-and-kill.md)
  follows it.

There is no system call that moves a capability between two running threads, and no message
carries one.

**Try it:** `main`'s narrow, the second `cap_narrow` of the run:

```
(gdb) break kickos::cap_narrow if $_streq(c->name, "main")
(gdb) continue
(gdb) p/x mask
$1 = 0xe
(gdb) p c->caps[0][1]
$2 = {obj = 0, obj_gen = 0, type = 1 '\001', seq_lo = 0 '\000', rights = 15 '\017', seq_hi = 0 '\000', gen = 0, badge = 0}
(gdb) p $P->endpoints.slots[0].recv_holders
$3 = 2 '\002'
(gdb) p $P->endpoints.slots[0].handout_holders
$4 = 1 '\001'
(gdb) finish
Value returned is $5 = 0
(gdb) p $k->currents.cells[0]->caps[0][1]
$6 = {obj = 0, obj_gen = 0, type = 1 '\001', seq_lo = 0 '\000', rights = 14 '\016', seq_hi = 0 '\000', gen = 0, badge = 0}
(gdb) p $P->endpoints.slots[0].recv_holders
$7 = 1 '\001'
(gdb) p $P->endpoints.slots[0].handout_holders
$8 = 1 '\001'
```

Before: two receivers (`main` and `sink`) and one `HANDOUT` holder (`main`). After: one
receiver (`sink`), and `main` still holds `HANDOUT`. The endpoint keeps its references:
a narrow changes rights, not possession. The badged copy is the one you already saw: slot 3 of
root's table in Stop 4, `badge = 1`.

**Why, and the contract:** [Reference: architecture, "Object model, capabilities & IPC"](../reference/architecture.md#object-model-capabilities--ipc)
(the authority pseudo-handle, and "Service publication"), [Book: Synchronous call/reply](../book/synchronous-call-and-reply.md)

## What you have seen

Root starts with every authority bit and no capability. The init creates objects, narrows its
own copies, starts a task per composition entry, and each task's first thread gets only the
authority its declaration names. From there, an application does the same on a smaller scale:
it creates an object (one reference, one full capability), starts a declared task (a group
named by a handle and owned by its creator), and spawns threads whose grants are checked
against the spawner's own table and can only narrow. The kernel checks everything before it
claims anything, claims in an order that can be undone, and seats the child's slots only when
nothing can fail any more. Closing reverses one capability: protocol, then a new generation,
then the reference.

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
   `kos_cap_narrow(ep, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_HANDOUT)` just before
   `kos_task_create`. Predict what the create answers and which check in `spawn_locked` says
   so. Check it with `break kickos::task_create_call`, `finish`, and the returned value
   (`-KOS_EACCES` is -13).
2. **Widening is refused.** Change the grant mask to `KOS_CAP_WAIT | KOS_CAP_SIGNAL` and keep
   `main`'s full capability. Predict whether the create succeeds, then predict `sink`'s slot 1
   rights before and after `bring_up`'s narrow. Then narrow `main`'s capability to
   `KOS_CAP_SIGNAL | KOS_CAP_TRANSFER` first and predict again.
3. **A stale handle.** After `kos_handle_close(prog)`, call `kos_handle_close(prog)` again.
   Predict the answer, then stop in `cap_lookup` and print `cap_handle >> 16` and the slot's
   `gen` to see which comparison refuses it.
4. **A second create.** Call `kos_task_create` for `sink` a second time while the first is
   running. Predict the answer from `task_decl_in_the_way` (`-KOS_EBUSY` is -16).
