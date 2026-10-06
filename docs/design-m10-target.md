<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M10.4: the target side

> **Status: ACTIVE.** This specifies what runs a composition on the target: the init, packaged
> drivers under it, the lookups, the system targets with their link-time asserts, and the golden
> runs. It builds on the kernel contracts of
> [`design-m10-kernel-share.md`](design-m10-kernel-share.md) and the host side of
> [`design-m10-composition.md`](design-m10-composition.md). `roadmap.md` assigns the steps,
> M10.4.2 to M10.4.9.

## The whole design in one paragraph

A system target carries the emitted table and the init. The init runs as root. At boot it
reserves what the table needs and creates every served endpoint and every watcher's notification.
It then scans the table in file order and starts each task whose `uses` servers are ready,
scanning again whenever a server becomes ready or a task dies. It reads the state of each task a
wake reports, counts a failed start as a death, restarts a dead task with the same grants while its count
lasts, tells the watchers, and ends the system when the task `ends` names ends. A task finds what
it was given by name: the lookups read the table, and ask the kernel where a window sits.

## 1. The init's walk

### 1.1 What the init holds from boot

The system target defines `kickos_init_entry`, which root calls after the app's constructors.
The kernel creates root at `KICKOS_PRIO_MAX`, so the constructors run there, and the kernel knows
no priority of the composition's. The init's first act, once it has checked the table's layout,
lowers root to the priority the table's header carries (`kos_thread_set_priority`), the
composition's `init: { priority: N }`, one above the kernel build's lowest where it states none;
a refusal panics. An image linking no system target lowers root to `KICKOS_PRIO_ROOT`, which
`cmake/sched_geometry.cmake` declares as `KICKOS_PRIO_MIN + 1`, before its constructors, its link group's `kickos_root_lower` being the
call `kickos_root_entry` makes first, which a system target's init defines empty. A thread at or
above the init's priority that never blocks starves it, which is the user's choice: the init
then handles no death and no ending, so a system whose `ends` task has ended does not shut down
while that thread spins. Its diagnostics go through `kickos::emit`, never stdio. At boot, in this order, it:

1. creates its notification (`kos_notify_create`) and binds it (`kos_notify_bind`); every watch it
   arms raises a bit of it;
2. reserves with `kos_ram_alloc`: its private block and each watcher's status block (1.6), each
   shared region, each packaged driver's ring block, and on a build that does not mask SP, each
   user task's stack. It self-grants its private block, each status block and each ring block. It
   never maps a shared region:
   the kernel hands every reservation out zeroed (section 7), which is how a region is zeroed
   once. A restart reuses all of them;
3. creates every served endpoint, a user task's and a packaged driver's, and narrows each at once
   to SIGNAL, TRANSFER and `KOS_CAP_HANDOUT` (`kos_cap_narrow` dropping `KOS_CAP_WAIT`), the
   console driver's endpoint at the end of its handover instead (2.1). The init never receives on
   one, so a caller gets `-KOS_EAGAIN` until a server receives, and the endpoint outlives every
   instance of its server;
4. creates each watching task's notification;
5. holds the AMP ports the kernel seated in root's table on a node of a partition. It delegates
   none: a composition has no field for a crossing until M10.5's partition build.

Then it scans.

### 1.2 Starting a task

A task starts when every endpoint it `uses` is served by an instance that has become ready. For a
user task the init:

1. mints a badged copy of the init's notification, the bit being the task's table index modulo 32
   (`kos_notify_badge`);
2. creates the task (`kos_task_create`): with no memory on a region board, and on a translating
   board with the task's stack block as its data region, which maps the block in the task's own
   space;
3. narrows the task's priority ceiling to its declared priority and, for a declared `core`, its
   core grant to that core (`kos_task_sched_grant`), so threads the task creates stay within its
   declaration;
4. arms the watch, `kos_task_watch(task, copy, served endpoint)`, with `KOS_CAP_NONE` for a task
   that serves nothing;
5. for a task taking lines, pins itself to the task's core (`kos_thread_set_affinity` on
   `kos_thread_self`, on a build of more than one kernel core) and claims each line edge
   (`kos_irq_claim`). The table carries no trigger, so a user task cannot take a level line; a
   packaged driver takes its trigger from its descriptor. Pinned, the init is starved by a
   thread at or above its priority on that core that never blocks, as any lower-priority thread is, timing being
   the user's to keep while M10 leaves the temporal half out of scope;
6. spawns the task's entry thread (`kos_thread_create`) with these `kos_thread_params`:
   - `entry` the library's trampoline and `arg` the task's table record, `self`;
   - its name, its declared priority, FIFO;
   - `windows`: every window-kind grant in table order, a device as `KOS_WINDOW_DEVICE`, a port
     range as `KOS_WINDOW_PORTS`, a map as `KOS_WINDOW_MEMORY` with the grant's flags, and for a
     watcher the status grant at its table position as the watcher's own status block,
     `KOS_WINDOW_MEMORY` read-only. Each grant's `window` field records its place in this list;
   - `caps`: its capability-bearing grants in table order, so the i-th lands at
     `KOS_SPAWN_DELEGATED_CAP0 + i` as the table records: a served endpoint with WAIT (from the
     HANDOUT the init holds), a used endpoint with SIGNAL, `/init/events` with WAIT, a line with
     WAIT;
   - `core_mask` and `authority` from the table, and `task` the one just created;
   - the stack: none on a build that masks SP, which gives the thread one whole stride, the
     composition's figure never handed over; elsewhere the reserved block at the declared `stack`;
7. closes the lines it claimed, resets its affinity and closes the badged copy.

A step from 3 on that fails, a failed start (1.6), unwinds what the start holds before the
death is counted (1.4): the init closes the badged copy, and from step 5 on also closes the
lines it claimed and, once pinned, resets its affinity.

The trampoline calls the entry with `self` and ends the thread with `kos_exit(0)` when the entry
returns; an entry ends with another code through `exit(n)` or `kos_exit(n)`. The entry thread
carries the task: its exit ends the task, as a fault of any member does (section 7). A task that
serves nothing is ready once spawned. A task spawned after a console driver's publish holds the
console at index 0, which the kernel seats.

### 1.3 The wait

The init waits on its notification for any bit (`kos_notify_wait`), with no bound. For each task
whose bit is set, in file order, it reads `kos_task_state` on the live handle it holds,
skipping a task pending restart or dead for good, which holds none. The kernel raises a task's
bit twice: when the task ends, setting the ENDED bit, and when every member's teardown sweep has
finished, setting the DEAD bit. The LIVE bit drops before that sweep, so a task with no member is
not yet a death. A task whose state carries the DEAD bit is a death (1.4). A task carrying the
ENDED bit and not the DEAD bit is ending: the kernel has stopped its members (section 7) and
the init waits for its DEAD bit; the task `ends` names ends the system instead (1.5), once every
other task the wake reports is handled. A task that is `KOS_TASK_READY` and not ended under the handle the init holds is
recorded as ready. The init then scans again. One wake may carry an end, a death and a readiness,
and tasks sharing a bit, so the state call says what happened and the bit only says where to
look; each task's state is read once per wake and kept for none. The generation is the task
handle's: a restart is a new handle, so a readiness of an instance that has since died is never
recorded, and a client waits for the instance its server runs now. A death and each start clear
the readiness the record holds, which keeps that true where the kernel hands a released handle
out again.

### 1.4 A death

A death is a task the wait finds dead, or a failed start (1.6). The init:

1. releases the instance once it is dead: `kos_task_kill` drops the init's hold, and on a dead
   task, every member swept, that frees the slot and the domain. A failed start is slain with
   `kos_task_slay` and a 1000 us timeout; the slay returns 0 only
   once every member's teardown sweep has finished, the instance then released by the slay itself,
   and one that times out keeps the init's hold, so its death arrives through the wait and the
   steps below run then. A restart therefore never reuses a ring block or a stack a member of the old
   instance still holds;
2. counts the death. With restarts left, it takes one and marks the task pending; with none, it
   marks it dead for good;
3. writes the task's record, then tells each watcher: it mints a copy of the watcher's
   notification badged with bit i, the task being the i-th it watches, raises it (`kos_notify`)
   and closes it. A watcher reads the counter already applied;
4. dead for good: closes the init's capability on the endpoint the task serves, which drops the
   last HANDOUT, so its callers get `-KOS_ECONNREFUSED`; marks every pending task that uses it,
   and theirs in turn, dependency-down, telling their watchers the same way and closing the
   endpoint each serves. A running client is left running, and marked so once its own death
   leaves it pending;
5. a pending restart starts at a later scan once its `uses` are ready, with the same grants: the
   endpoint, notification, stack and ring block the init kept, and lines the kernel freed with the
   dead instance.

The end of the task `ends` names ends the system instead (1.5).

### 1.5 Ending the system

`ends: never` keeps the init waiting until reset, whatever has died. `ends: <task>` ends the
system when that task ends: its entry thread returns, calls `exit` or `kos_exit`, or is killed or
slain, or any of its members faults. The init acts on the ENDED bit and does not wait for the
DEAD bit. By then the kernel has stopped every other member of the task, a thread spinning without a system call
included, so a plain app whose `main` returns ends the system whatever its other threads were
doing. It reads the status the
kernel latched with `kos_task_exit_status(task, &status)` (section 7): 0 when the entry returned,
the code passed to `exit` or `kos_exit`, any `int` and negatives included, `KOS_EXIT_FAULT` after
a fault, `KOS_EXIT_CANCELLED` when the entry was cancelled. An `ends` task marked
dependency-down, or whose start fails, never runs its entry, and the system ends with
`KOS_EXIT_CANCELLED`. Where a console driver holds stdout the init drains it with two
zero-length `kos_send_timed` on `KOS_CAP_STDOUT`, each bounded by `KOS_DRV_HANDOVER_PROBE_US`
(1 s), the second completing once the driver has taken the first; a send that fails ends the
drain, so a hung console driver delays the ending by at most 2 s and never hangs it. It then
calls `kos_shutdown(status)`, which its `KOS_AUTH_SYSTEM` allows. Nothing is torn down first, so
no thread is classified as app or infrastructure. Admission refuses `restart` on the ending task.
`kickos_main` is `exit(main(argc, argv))` running as the entry thread, so `main`'s return ends the
system with its code.

`kos_shutdown` drains the kernel console and calls the chip's shutdown with the status:

| board class | what ending does |
| --- | --- |
| sim | the host process exits with the status |
| QEMU `virt`, A53 | a semihosting exit: QEMU exits with the status |
| QEMU q35 | COM1 prints `KICKOS-EXIT status <n>`, then the debug-exit port ends QEMU |
| silicon | interrupts masked and the core halted, or with `KICKOS_SHUTDOWN_TO_BOOTLOADER` a reboot into the chip's bootloader |

### 1.6 The blocks and their failures

The init keeps a private block and, for each watcher, a status block. A watcher's status block
holds one record per task it watches, in `watches` order, and its `status` grant maps it read-only
into that watcher alone, so no watcher reads the record of a task it does not watch. A record
holds a sequence count, the deaths, the restarts left, alive and dependency-down; when a task's
record changes the init writes it into every watcher's block that holds it. The private block is
the init's own and never mapped: a record per task in table order holds the current instance's
handle, the instance that last became ready, its endpoint and notification capabilities, its
reservation, a user task's stack or a packaged driver's ring block, from which each start fills
the instance of section 2, and for a watcher where its status block sits; after the tasks'
records, one record per shared region holds where the init reserved it, which each spawn mapping
the region names. A status block's size is the count of tasks its watcher watches times its record
size and the private block's the task count plus the shared region count times its own, each
record size declared by `cmake/init_geometry.cmake`, beside `cmake/driver_geometry.cmake`, and
exported by the manifest's `init` section, and the init asserts both. The init alone writes them.
A status record's
count is an `Atomic<uint32_t, Order::RELEASE>` and each field an `Atomic` at `Order::RELAXED`
(`system/include/kickos/sys/atomic.h`). To write a record the init stores the count odd, issues a
release fence, writes the fields, then stores the count even with release ordering. A write keeps
the count's parity, so the init clears each record of a status block it reserves, the count 0 and
every field 0, before its first write, and reads nothing of a reservation it has not written.

A refusal of a kernel call in the walk is a defect admission missed, so the init panics with the
step, the task and the answer named, the answer as its errno number: the init carries no prose
errno table. At boot, before it reserves anything, the init panics as well on a table admission
would not have emitted: a window placed at or past the task's window count, a capability slot
past its `cap_grant_count`, a spawn wider than the build takes, and a line taker declaring no
core on a build of more than one kernel core, a user task carrying a ring block, and a port range
bound to a packaged driver's window role, which its descriptor takes as a device window. It
reports, one line each, a failed start with its step and answer, a death it releases, and a slay
that leaves members, each once the slay or the kill has run,
since a console driver's failed start may still hold stdout. It prints the lines a pass makes once
the pass's deaths are handled and the restarts they allow are started, so a stalled console delays
no recovery. Each line is one send on stdout bounded by `KOS_DRV_HANDOVER_PROBE_US`, what it leaves
going to the kernel console, so a console driver stalled or holding members past its slay never
parks the init. Three results are not refusals:

- **`kos_notify` answering `-KOS_EALREADY`**: the watcher has not taken the bit yet, which is
  success; it reads every record when it does.
- **`kos_irq_claim` answering `-KOS_EAGAIN`**: the line is retiring from its last holder, which
  every restart of a line taker meets on a build of more than one kernel core. The init sleeps
  1 ms and claims again, at most 100 times, then counts a failed start.
- **A failed start**: any step of a user task's start after `kos_task_create`, or a packaged
  driver's `START` returning nonzero, its handover probe included. The task is not killed by the
  step that failed; the init counts it as a death of that instance (1.4), so a task failing before
  its first spawn is restarted while its count lasts and then marked dead for good, its watchers
  told each time.

## 2. Packaged drivers through their descriptors

A driver task's table entry names its `START`, declared on `kickos_add_driver`. To start it the
init mints the badged copy of 1.2's first step, calls `START` with a `kos_service_cfg` it fills
from the table, and closes the copy when `START` returns. The cfg carries the task's name, its
priority, its first window role's base and size, `hz` and `addr` 0, and a trailing field, the
instance, the bring-up taking the kind from its descriptor. The size assert on `kos_service_cfg`
counts three pointers, and the bring-up refuses a cfg whose reserved bytes are not zero. A service
list leaves the instance null; M10.5 deletes the lists. The instance, `struct kos_driver_instance`
in `<kickos/sys/driver_service.h>`, is filled at each start from the init's record of the task
and its table entry, so it adds nothing to the private record: the endpoint the init created at
boot and the ring block of 1.1, both kept across restarts in the record, the table's line numbers
by role, the core mask, whether the table names it the console driver, which the bring-up refuses
unless its descriptor hands the console over, the badged copy of this start, and the task handle the bring-up writes
back as soon as it creates it. The badged copy is not kept: the init passes it for that one start
and closes it when `START` returns.

The table carries what the init needs of a driver that its catalogue entry states: the task's
`block`, the ring block's bytes, which the init reserves at boot, and its `flags`, whose
`KOS_TABLE_TASK_CONSOLE` names the driver that takes the console, whose endpoint the init narrows
at the end of its handover rather than at boot and drains at the ending (1.5). A driver task's
line grants are emitted in the order of its line roles, which is the order its descriptor numbers
its lines in.

`bring_up` in `user/src/driver_service.cc`, given an instance, runs in the init's thread and:

1. lays out the kept ring block (`block_init`), creates the task with it (`kos_task_create`), and
   narrows the task's priority ceiling to its priority plus its highest thread offset and its core
   grant to the declared core;
2. arms the watch with the badged copy of this start, the ready endpoint being the instance's;
3. under handover, publishes the instance's endpoint (2.1);
4. pins itself to the instance's core and claims the table's lines at the descriptor's trigger,
   refusing a line its descriptor numbers differently; M10.5 removes the number from driver
   source. A claim answered `-KOS_EAGAIN`, the line retiring from the instance before, is tried
   again as 1.6 states;
5. creates the notification and binds each line to it;
6. spawns each thread pinned to the declared core, with its window and its descriptor
   capabilities, the endpoint coming from the instance;
7. polls the readiness barrier between the spawns the descriptor names;
8. closes the lines and the notification, the threads holding their own;
9. under handover, finishes it (2.1).

The endpoint is the instance's under either posture, so given an instance `out_ep` is neither
checked nor written. Before step 1 the bring-up refuses an instance that is not its descriptor's:
a ring block of another size, a block its descriptor types (the init's self-grant carries no
memory type), or lines its descriptor numbers differently. A step that fails closes what
`bring_up` made, leaves the task and the endpoint to the init, and returns its code, a failed
start (1.6). A driver's first descriptor thread is its entry thread.

Given an instance, a driver thread that fails ends its task by trapping: the kernel's fault path
stops the rest of the group at once and the death reaches the init, whichever thread failed. The
init never kills a live instance, and a thread its task's end or a slay stops runs no further code,
so under the init every failing call is a failure. On a kernel build without fault isolation
(`KICKOS_FAULT_ISOLATION` off, the LX6 and the parts with no privilege split) every fault ends the
system, a driver's trap included, and nothing falls back to `exit`: the manifest exports
`fault_isolation` and admission refuses `restart` on a packaged driver there
(`restart.no-isolation`). That replaces the
`kos_panic` on a failed open in `system/driver/xmc4800/xmcssc/xmcssc.cc`,
`system/driver/mk64f/k64dspi/k64dspi.cc` and the IRQ thread of
`user/include/kickos/sys/uart_service.h`, and the `exit(0)` that ends a UART driver's IRQ thread
and its console receiver (`user/src/uart_service.cc`) once a wait or a receive fails; the
receiver is not the entry thread, so its exit would end it alone and leave the task alive. A
thread learns its posture from its spawn: given an instance, `bring_up` sets bit 0 of each
thread's arg, which no declared arg sets, a ring block and a window base being aligned, and every
descriptor thread's entry first calls `kickos::driver::thread_start`, which records that posture
in the thread's own space, where `trap_under_init` reads it, and returns the declared arg. On a
service list no arg carries it, and a failing driver thread panics until M10.5, and `bring_up`, given no instance, creates its own endpoint and kills
its task on failure. `kos_service_bringup` and `kos_service_list` stay the service lists' types, which the init never
reads.

### 2.1 The console handover

A console driver restarts as any packaged driver does, and `bring_up` repeats the whole handover
at every start of it:

1. publishes the endpoint (`kos_console_publish`) before claiming the UART line, the publish
   being what frees the line from the kernel's handler. The publish requires HANDOUT on the
   init's capability, refuses one without it with `-KOS_EACCES`, and seats WAIT on it when it is
   missing. At the first start the init still holds all four rights it created the endpoint with;
   at a restart it holds SIGNAL, TRANSFER and HANDOUT, and the publish seats WAIT and makes the
   endpoint receive, so every start goes on from the same state, the init holding WAIT and
   HANDOUT (`docs/reference/console.md`, "Capability handover");
2. claims, spawns up to the readiness barrier, polls it, spawns the receiver;
3. narrows the init's capability to SIGNAL, TRANSFER and `KOS_CAP_HANDOUT`, dropping its receive
   right, then probes once with a timed zero-length send on `KOS_CAP_STDOUT`, which completes
   once the receiver takes it. A probe that does not complete is a failed start.

A step that fails narrows the init's capability the same way before it prints or returns:
`kickos::emit` is a blocking send on index 0, which parks while a WAIT holder exists with no
receiver, so the init would otherwise park on its own endpoint. `console_handover_finish`
narrows given an instance; on a service list it closes the capability, as
`user/apps/common/drvdeath/main.cc` and `tests/unit/drvbringup/bringup_unwind.cc` expect.

A driver that dies before step 3, or a start that fails before its receiver exists, is reclaimed
when the init drops its receive right, the last WAIT going, so the kernel console comes back; a
driver that dies after it is reclaimed at its own death. With HANDOUT retained, a dead console
receiver answers `-KOS_EAGAIN`, not `-KOS_ECONNREFUSED`: the probe sees `-KOS_EAGAIN`, and a
writer's `_write` sends that chunk to the kernel console and keeps its capability. The next start
publishes the endpoint again, and a write parks until the new receiver takes it; once the count is
spent the init closes its own capability, the last HANDOUT goes, the next write gets
`-KOS_ECONNREFUSED`, and the writer closes index 0. The comment on `console_handover_finish`
states that.

### 2.2 The stop step

A restart needs no stop hook from a driver. Its threads hold their windows, lines, notification and
capabilities, which the kernel frees at their death, reclaiming the console when a console driver's
last receiver goes. The endpoint, the ring block and the task record are the init's, kept for the
next start, and the release of 1.4 is the stop, the same for a driver as for a user task. The next
start lays the block out again and reprograms the device.

## 3. The cost model

`design-m10-composition.md`, "What one task costs", is what M10.3's admission counts, and the init
spends exactly that. Where the walk above cannot, the model and the tool change together, each
change with its arm:

| term | counted today | what the init spends | the change |
| --- | --- | --- | --- |
| the init's peak | the holdings of a file-order prefix of starts, then each restart | starts out of file order when a server is late | what it keeps for life plus the most one start holds, first start or restart |
| a start's copies | none for a user task, two for a driver with a notification | one badged copy of the init's notification at every start; a driver's bring-up one copy of the driver's notification while it binds each line, and at each thread's spawn the badged copies that thread's capabilities name | one per start, and for a driver with a notification the most one step of its bring-up holds: one while it binds a line, or the most badged copies one thread's spawn mints, which `kickos_add_driver` declares per thread and the catalogue carries |
| a console endpoint | held until handover ends | kept with HANDOUT for life, its receive right dropped at the end of handover | kept as a retained endpoint is |
| domains on a region board | one per task with a ring block or a shared map | a map is a window, so only a ring block brings a domain | one per ring block |
| the init's self-grants | not counted | its private block, each watcher's status block and each ring block, held for life | on a region board within what root's region set holds past its static regions and its stack, `KICKOS_MPU_MAX_REGIONS` less those, which the build derives from root's linker script and exports in the manifest's `init` section as `free_regions`; a self-grant spends a region whether or not the build enforces, root being unprivileged on every build; on a translating board a self-grant takes the range its reservation holds |
| reservation records | not counted | its private block, each watcher's status block, each shared region, each ring block, and on a build that does not mask SP each user task's stack, once for life | against `KICKOS_RAM_OWNER_SLOTS` on a region board that enforces its unit, the only build that keeps the table; on a translating one against `KICKOS_ASPACE_RANGES` in the init's space, beside its image's two ranges and its root stack's; both knobs exported in the manifest's pools |
| a task's own space | not counted | on a translating board two range records for its image, one per thread's stack, one per window, and one for its data mapping: a user task's stack block, on which its one thread runs, or a driver's ring block, its threads running on default stacks | within `KICKOS_ASPACE_RANGES` for each task |
| the arena | idle, root, heap, regions, then each task's ring block and stacks | the boot stacks, then its reservations in the order of 1.1, then the kernel's default stacks, one size, in start order | that order, the heap leaving the replay for the link, which places it below the arena |
| a watcher's windows | its devices and maps | its own status block too, read-only | the status grant the emitter writes, one window more, sized the count of tasks it watches times the manifest's status record size and rounded as a region is |
| the ending and the watch bits | not refused | one exit ends the system, and a watch is one bit | `restart` refused on the task `ends` names, and a task watching more than 32 tasks refused |

The rest is exact as counted: tasks, threads, endpoints, notifications and IRQ bindings in their
pools, a user task's budgets, the spawn grants, a thread's windows and the stack floor.

The status grant is a grant kind of its own, `status`, named `/init/status`, which the emitter
writes after `/init/events` for every task that watches. Every window-kind grant (`window`,
`ports`, `region`, `status`) records its place in the spawn's window list in the grant's `window`
field, which takes the reserved field after `target`. The header carries the init's priority,
which grows it to 32 bytes. Each changes the layout, so `KICKOS_TABLE_VERSION` is 3. A packaged
driver's grants record no window place, its `Descriptor` placing each thread's window.

## 4. The lookups

The library is part of `libkickos_user`, its declarations in `<kickos/sys.h>`:

```c
typedef struct kos_table_task kos_self_t;     /* <kickos/sys/table.h> */

typedef struct { void* addr; uint32_t size; } kos_window_t;   /* read through the accessors */
typedef struct { kos_cap_t cap; uint16_t index; } kos_line_t;  /* index: the line within its device */

struct kos_task_status
{
    char const* name;      /* in the table */
    bool alive;            /* running, or a restart of it is coming */
    uint16_t deaths;
    uint8_t restarts_left;
    bool dependency_down;  /* never started: a server it uses is dead for good */
};

kos_cap_t kos_grant_endpoint(kos_self_t const* self, char const* name);
kos_cap_t kos_grant_notify(kos_self_t const* self, char const* name);
kos_window_t kos_grant_mmio(kos_self_t const* self, char const* name);
kos_window_t kos_grant_mem(kos_self_t const* self, char const* name);
kos_window_t kos_grant_ports(kos_self_t const* self, char const* name);
kos_line_t kos_grant_irq(kos_self_t const* self, char const* name);
void* kos_window_addr(kos_window_t window);
uint32_t kos_window_size(kos_window_t window);
int kos_task_status(kos_self_t const* self, uint32_t i, struct kos_task_status* out);
```

- **A name** is a grant's `name` among `self`'s grants: the path, or the name the entry renamed it
  to. Each lookup matches its kinds: an endpoint served or used, a notification, a device window,
  a region, a port range, a line.
- **A capability** answers `KOS_SPAWN_DELEGATED_CAP0` plus the grant's place among the
  capability-bearing grants, from the table, with no system call.
- **A window** asks the kernel for the window at the place the grant's `window` field records
  (`kos_window_get`, section 7) and checks the answered window's kind and size against the
  grant, a mismatch answering an invalid handle. Its address is where this thread reaches it: the
  physical base on a region board, where the kernel mapped it on a translating one, and the first
  port of a port range. Its size is the declared one, a region's rounded as admission rounds it.
- **An absent grant**, a null `self`, or a window the thread does not hold answers an invalid
  handle: `kos_window_addr` null, `kos_window_size` 0, `KOS_CAP_NONE`, a line of `KOS_CAP_NONE`
  at `KOS_TABLE_NONE`.
- **Capabilities and windows are per thread**, so the lookups answer for the task's entry thread,
  the one the init spawned. A thread the task creates reads `self` but holds only what its own
  spawn delegates, which the entry passes it.
- **`kos_task_status`** answers for the i-th task `self` watches, the one bit i of `/init/events`
  stands for, and `-KOS_EINVAL` past the last. It reads record i of the watcher's own status
  block through the status grant, with no system call beyond finding the window and nothing asked
  of the init. It loads the
  record's count, issues an acquire fence, copies the fields, issues an acquire fence and loads
  the count again. A count odd or changed sleeps 1 ms and reads again, at most 100 times, then
  answers `-KOS_EAGAIN`. The init holds a count odd for a few stores, so the bound is never
  reached while the init runs.

The table and `self` grant nothing: a task substituting another entry finds slots and windows it
does not hold.

## 5. System targets

**`KickOS::kernel`** is `kickos_cxx`, full C++ over newlib with `main` renamed,
without the init provider, root's lowering, the service list and the pin map in its archive group. It requires the
symbol `kickos_link_one_system_target` at the link (`--require-defined`), which every system
target's emitted table defines: the init's objects are the same files in every system target,
and both CMake's link line and the x86_64 image link name each once. Linking no system target fails
the link naming that symbol, and linking two fails it as a multiple definition of it. Until that
check, `KICKOS_USER_HEAP_SIZE` reads 0 where no system target defines it: an implicit script
`KickOS::kernel` carries provides it, and the x86_64 image link defines it. A system target's
asserts refuse any value but its own heap, so one that defines none fails its link.

**`kickos_compose(<system> <composition.yaml>)`** runs the host tool at configure, under `uv`
from the package's `compose` folder, against the package's manifest. It writes the table's C
source, a linker script of asserts (section 6), and a CMake fragment listing the drivers the
composition names, whether `kickos_main` is named, and the heap. A refusal, the tool exiting 3,
fails the configure with the tool's `<file>:<line>: <rule>: <message>` lines; any other failure
fails it saying the tool could not run, uv and Python 3.12 or newer being what it needs. A change to the composition, the manifest, a
description or the tool configures again and runs the tool; a configure with none of them changed
does not. It then defines:

- `<system>_table`, an object library of the emitted table;
- `<system>`, an interface library that depends on `KickOS::kernel` and carries, ahead of the
  kernel's group on the link line: the table's objects; the init's objects (`KickOS::init`); those
  of `KickOS::main` where `kickos_main` is named; each named packaged driver's archive, and the
  libraries its `CLIENT` declares for the tasks that use it; the heap as `--defsym=KICKOS_USER_HEAP_SIZE=<heap>`, the
  composition's `heap`; and the asserts script as a link input.

Objects rather than archives make a second system target a duplicate definition. A missing entry
fails the link naming it, the table declaring each entry `extern`. A packaged driver's client
library, `kickos_spi_proxy` for `xmcssc`, which the golden sensor calls through
`<kickos/driver/spi.h>`, is declared by `CLIENT` on `kickos_add_driver`, carried in the catalogue's
`client`, and named in the fragment as `KICKOS_COMPOSE_CLIENTS`. On a board no emulator runs, the
golden system as written is linked against the package instead of run.

The user's CMake stays plain:

```cmake
find_package(KickOS REQUIRED)
add_executable(app main.c)
target_link_libraries(app PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(app)
```

**`KickOS::system_default`** is `kickos_compose` run by the kernel build on its export's default
composition, after the manifest. Its objects and script are installed and the target exported.
Every board has a default composition, and the configure refuses one without it. The default
composition names `entry: kickos_main` and `ends` on it, and states the board's heap.

**x86_64** links as every other board does since M10.5 (`docs/design-m10-fleet.md`, section 7):
`KickOS::kernel` carries the kernel's group without the old init and with the C++ runtime, and
each system target its objects, archives, heap and script, as usage requirements.

**The namespace.** The targets are `KickOS::kernel` and `KickOS::system_default`, exported by
`install(EXPORT KickOSTargets NAMESPACE KickOS::)`. The targets take `EXPORT_NAME` `kernel`,
`system_default`, `init` and `main`; every exported target gets an in-tree `ALIAS` of its exported
name, and the package's functions name targets that way: the arch check and the class-backend lines
`KickOSConfig.cmake` writes. `examples/oot-app/` and `examples/oot-mcu-app/` link `KickOS::kernel`
and `KickOS::system_default`.

## 6. The link-time asserts

The asserts script holds `ASSERT`s and the arena replay's cursor, an implicit script beside the
chip's. The tool writes its constants from the manifest, and its symbols are the chip script's,
spelled with the manifest's `symbol_prefix`:

1. **The thread-local share of a stack.** For each user task on a build that does not mask SP, its
   `stack` is at least `KICKOS_MIN_STACK_SIZE` plus `__kickos_tls_carve`, the block a stack pays
   for the linked image's thread-local data, which `arch/common/sections.ld.h` defines from the
   expression its carve assert already reads. Where SP is masked every thread gets one stride, and
   the chip script's own assert on `KICKOS_USER_STACK_SIZE` is the check. Arm: a composition whose
   stack sits at the floor, linked with a thread-local object.
2. **The arena fit, on a region board.** From the arena base the chip script places, the idle and
   root stacks as `arch/common/boot_arena.ld.h` places them, then the private block, each
   watcher's status block, the shared regions, the ring blocks, the reserved stacks and the
   default stacks, each rounded and aligned as `arch_ram_alloc` places it, end at or below the arena's end. The replay is admission's, with
   the image's static footprint known. Arm: a composition one stack past the arena.
3. **The heap.** Where the chip script carves `.userheap` it sizes it by the
   `KICKOS_USER_HEAP_SIZE` link symbol, and the span holds the system's heap, the composition's
   `heap`. On an enforcing region board the heap is the app
   window's pad, so the assert checks that heap against the pad and never grows it. Arm: a heap past the Relax Kit's pad.

Each message names the task or the figure and the knob that would have to grow, and cites the
composition by its path. The heap's says what a consumer can change: a smaller `heap` in the
composition, or a package built to carve more; the one tying the link symbol to the system says to
link exactly one system target and define no heap of the app's own. Every chip script
that reads `KICKOS_USER_HEAP_SIZE`, about twenty of them, reads it as
a link symbol rather than a preprocessor macro, and `arch/CMakeLists.txt` does not pass it to the
preprocessor; the `kickos` leaves define the symbol from the knob until M10.5 deletes them.

## 7. The kernel's share for the init

Four changes, each with its self-test arms:

1. **A window asked by its place.** `kos_window_get(index, &window)` answers the calling thread's
   index-th spawn window, its base being where the thread reaches it, its size and kind and flags
   as spawned. It replaces the base-keyed query: the base of a memory window is the spawner's
   reservation, which the child cannot know. The accessor takes the name `kos_window_addr`, and
   every caller of the base-keyed query moves. A window's place is an explicit ordinal the spawn
   records with it in each of the three stores, the region set on a region board, the
   `VR_WINDOW` range record on a translating one and the port list on x86, and is never derived
   from the order of a store.
2. **A task ends with its entry thread.** A task's entry thread is the first thread spawned into
   it. The task ends when that thread dies by any cause, a return, `exit(n)` or `kos_exit(n)`, a
   fault, or `kos_thread_kill` or `kos_thread_slay` of the entry alone, or when any member
   faults. The kernel then latches the status, sets the ENDED bit, raises the creator's watch and
   stops every other member at once with `CANCEL_SLAY`, whether or not a creator holds the task:
   as a process ends, no member runs a further instruction of its own, a member parked in a wait
   or spinning without a system call alike, and no member gets a cleanup window. A privileged
   member is killed instead (`CANCEL_KILL`), as `kos_thread_slay` refuses one, and stops at its
   next system call. Another thread's exit or cancel ends that thread alone. A task that has
   ended takes no new member, a spawn into it answering `-KOS_EBUSY`, so a restart is always a
   new task. `kos_task_kill` stops a group the same way at its creator's request, and
   `kos_task_slay` stops it and waits until every member is swept.
3. **The end and death bits and the exit status.** `kos_task_state` carries the ENDED bit, set
   when the task ends, and the DEAD bit, which `task_report_death` sets, raising the creator's
   watch again, once every member's teardown sweep has finished; the LIVE bit drops before the
   sweep. `kos_task_exit_status(task, &status)` writes the latched status and answers 0 once the
   task has ended, `-KOS_EBUSY` before, for its creator only. The status is 0 when the entry
   returned, the code passed to `exit` or `kos_exit`, any `int`, `KOS_EXIT_FAULT` after a fault,
   and `KOS_EXIT_CANCELLED` when the entry was cancelled. The creator observes `KOS_EXIT_FAULT`
   through it, though a join does not.
4. **Every reservation zeroed.** `kos_ram_alloc` on a region board clears its block once its
   `IrqLock` is released, as the frame pool already clears a translating board's.

## 8. The golden runs

**In CI.** On the presets that name `KICKOS_GOLDEN_COMPOSITION`, `qemu-x86_64-smp2` at two cores and
`qemu-arm64-smp` at four, a gate installs the build's package in a scratch prefix as
`tests/integration/check_oot_export_mcu.sh` does, configures `examples/composition/` against it with
the package's toolchain file, builds `sensor_system`, and runs it under `poll_image` from
`tests/lib/gate.sh`. Each golden system as written passes on `sensor: <n>` for three distinct values
of n, each an intact line: above one core a split line is skipped rather than matched, since a
foreign line ending in a digit could complete it. Once the run has ended the gate checks the whole
capture for no `sensor: no reading`, `sensor: restarting`, `died`, `gone for good`, `measurement
frozen` or `health:` line and no panic or fault banner. q35's `QEMU_TIMEOUT` covers OVMF's boot plus
three distinct CMOS seconds.

**The restart witness.** None of the golden tasks dies on its own, so a test-owned project beside
the gates owns copies of the example's `app.cc` and `health.cc`, making the same calls and the same
decisions and ending each numbered line in the endpoint or region it came from, so above one core a
foreign line cannot complete one across a split. It reuses the example's `sample.h`, with a test
sensor that serves five readings per instance and returns, under a copy of the board's golden
composition naming it with `restart: { max: 2 }` and a `core`. Its entry first creates a thread
above its declared priority and, on more than one core, one on another core, and prints each
refusal. A restarted instance sleeps 500 ms before its first receive, so the app prints `sensor:
restarting`, its call answered `-KOS_EAGAIN` until the server first receives. It passes on the
refusals, then the readings in order, each instance's last reading before `health: sensor died, last
reading <n> in /shm/history`, `health: sensor restarted (1 deaths, 1 restarts left)`, the same at 2
deaths and 0 left and `health: sensor is down for good, running degraded`, and before the app's
`sensor: restarting` ahead of the next instance's first reading or its `sensor: gone for good`.
Health's lines are ordered against the app's only where the death orders them: above one core the
two print at once. It runs on both golden presets and on `qemu-riscv`, a region board whose SP is
not masked, which runs the reserved stacks, `stack_base`, a region board's window order and the
passing side of the stack and arena asserts; every QEMU M-class preset masks SP. `qemu-riscv` has no
golden, so its composition is a new one, over the board's own chip and board files.

**The driver restart witness.** No golden driver dies on its own either, so test-owned packaged
drivers, declared on `kickos_add_driver` with no window and no line, are each named with
`restart: { max: 1 }` beside a watching user task. The first serves five requests per instance and
then exits; the second serves from a thread that is not its entry thread and traps after five
requests. Each passes on the driver's death, its restart through its descriptor, a second death, and
its callers' `-KOS_ECONNREFUSED` once the count is spent, each death told to the watcher after its
instance's fourth answer, the client printing the fifth only once the reply reaches it, and not
ordered against the next instance's answers. Above one core each client shares the watcher's core
below its priority, so the watcher reads each death before the next instance can die. The second
also passes on the trapping thread's fault report in each instance. A third, whose `START` fails
before its first spawn, is restarted once and then marked dead for good, its watcher told each time,
and its client, which the init never starts, marked dependency-down. Its `START` first parks the
init's thread on the core its watcher shares, the task's declared one above one core, where the init
resumes only once nothing above its priority is ready, so the watcher has read the first failed
start before the second. Each client line ends in the endpoint it called, so above one core a
foreign line cannot complete a count across a split. All three run in CI on `qemu-arm64-smp` and
`qemu-riscv`, the presets that name its system in `KICKOS_DRIVER_WITNESS_SYSTEM`: the drivers live
in `tests/drivers`, which only a build with `KICKOS_TEST_DRIVERS` adds to its catalogue, a gate
holding every other package to none of them, and the system in `tests/integration/driver_witness`,
built against the installed package. The same run measures the init's depth on root's stack with the
composition witness's fill and scan, the init now starting and restarting packaged drivers. The
trapping thread's fault report witnesses the trap's encoding on AArch64 and RV32; on the other
arches with fault isolation a selftest arm traps a thread of a task of its own and reads
`KOS_EXIT_FAULT`, run under QEMU for ARMv7-M, ARMv8-M, RV64 and x86_64, and on the sim. ARMv6-M's
emulated board, the micro:bit, has no privilege split and so no fault isolation, and RX has no
emulator here, so on both the arm runs only in a silicon capture, ARMv6-M's on the picopi; the LX6
has no fault isolation, so a trap there ends the system.

**The console restart witness.** No packaged console driver dies on its own either, and the Relax
Kit's `xmcuartirq` runs only on silicon, so `tests/drivers` adds `testpl011`, a console driver over
a PL011's window that serves five writes per instance and then exits, each instance marking its
start and its end on the wire itself. A test composition on `qemu-arm64-smp` grants it the board's
console device, names it `stdout` with `restart: { max: 1 }`, and gives the system a writer printing
thirty numbered lines whose return ends it. It passes on every line exactly once and in order, a
line between each instance's marks, a line after the second end, which only the kernel console
given back at that death can carry, both deaths reported by the init, and status 0
(`tests/integration/check_console_restart.sh`, the system in `tests/integration/console_witness`,
named by `KICKOS_CONSOLE_WITNESS_SYSTEM`).

**The line taker.** A test composition on `qemu-arm64-smp` gives a user task the PL031's window and
its `alarm` line on core 1, the task arming the match interrupt and printing when it arrives.

**The init's priority.** A test composition on `qemu-riscv`, one core, states `init: { priority:
5 }` between a task at 6 and one at 4, both watching a pulse at 7 that sleeps and ends and that
the init restarts once. Each spins across one of the pulse's ends reading its status record: the
task at 6 reads no death arrive, the init staying off the CPU, and the task at 4 reads one
arrive, the init preempting it to report it. Stating no `init` turns the second red, and an init
that never lowers itself the first.

**The default system.** A board builds `KickOS::system_default` when it has a libc and a
`boards/<board>/composition.yaml` (`user/apps/common/CMakeLists.txt`), which every board has, the
sim included. On the sim and on every preset an emulator runs, a plain C `main` printing a line and
returning 3, linked against `KickOS::kernel` and `KickOS::system_default`, prints the line and
ends with status 3, as does one whose `main` returns 3 while a thread it created spins at
`KICKOS_PRIO_MIN`, below the init's priority, which no default composition states and so is
`KICKOS_PRIO_MIN + 1`; one that faults ends with `KOS_EXIT_FAULT`. Build-fail tests hold the link
messages: no system target,
two, a missing entry, and each assert's arm.

**The Relax Kit.** `tools/bench/bench.sh` builds the example and the restart witness against the
`xmc4800-relax` build's package and captures them through `tools/bench/bench-capture.sh`. With
`JUDGE` naming a gate script, the tool runs it over the capture it took with `KOS_CAPTURE` set, the
script judging the capture's last boot by the same lines: `check_golden_system.sh` also by each
packaged driver's up line before the first reading, `[xmcuartirq] device up (IRQ TX)` and
`[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)`. No jumper and no external
part: the SPI bus loops back inside the channel, and the console is the on-board J-Link's. The
init's stack depth there is measured at `KICKOS_ROOT_STACK_SIZE`, the root stack filled and scanned
as `kstack_high_water` scans a kernel block, its base read as `__aeabi_read_tp` reads it, SP masked
down to the stride configure holds that size to; CI links that restart witness against the package
(`xmc4800_relax_restart_witness_link`), and the board raises the reservation where the depth leaves
no margin.

**The arms.**

- Section 7, the kernel self-tests on every preset: `window_get` answering the index-th window and
  `-KOS_EINVAL` past the list, a translated address differing from the physical one; `task_end`
  with an entry return (0), `exit(5)` (5), `kos_exit(-3)` (-3), a member's fault
  (`KOS_EXIT_FAULT`), a killed and a slain entry (`KOS_EXIT_CANCELLED`) each stopping its parked
  sibling,
  a non-entry exit that leaves the task running, an entry exit stopping a sibling that spins
  without entering the kernel and a member's fault stopping both a spinning sibling and a parked
  entry that never runs its own code again, the DEAD bit then set with the status latched, a
  creator's kill stopping a parked and a spinning member the same way, cancelled members that
  change no latched status, and a spawn into an ended task answering `-KOS_EBUSY`;
  `task_exit_status` answering `-KOS_EBUSY` until the ENDED bit is set and `-KOS_EPERM` to a
  non-creator; a region board's
  `kos_ram_alloc` reading zero from a block its last holder dirtied.
- Section 2.1 (M10.4.8): a console driver failing before its receiver spawns leaves the init able
  to print; a failed handover probe is a failed start; a dead console receiver answers writers
  `-KOS_EAGAIN`; a service list still closes the init's capability; a console driver's restart
  repeats the handover on the init's endpoint, and a failure during it narrows before it prints,
  the kernel console back by then; and the selftest's `console_publish_handout` and
  `console_publish_narrow`: a capability holding neither WAIT nor HANDOUT refused, a WAIT-only
  capability refused, a publish through HANDOUT seating WAIT and handing the console over, and the
  console back when the driver's last WAIT goes and when the publisher's narrow drops the one the
  publish seated.
- Section 3, a mutation arm per row in the tool's tests, the 32-watch row stated per watching
  task: a task watching 32 tasks is admitted and one watching 33 is refused.
- The lookups (M10.4.4), host tests against emitted tables: each kind, a renamed grant, an absent
  one, a watcher's `/init/status` and `/shm/history` at their `window` places, and the status read
  against a writer held odd, answering once it goes even and `-KOS_EAGAIN` past the bound.
- The walk (M10.4.5), host tests against a scripted kernel: a death and a readiness in one wake, the
  generation check ignoring a dead instance's readiness while its clients wait on `-KOS_EAGAIN`,
  dependency-down and its watchers, an `ends` task dependency-down ending with
  `KOS_EXIT_CANCELLED`, an `ends` task ending the system at the ENDED bit while a sibling spins,
  an ended task never slain and released at its DEAD bit, the wait never bounded,
  `kos_notify` answering `-KOS_EALREADY`, `kos_irq_claim` answering
  `-KOS_EAGAIN` within the bound and past it, a task without a live handle skipped by the wait,
  a user task's failed start after `kos_task_create` counted as a death once its step's unwind
  is done, a stdout drain against a console that never receives ending within its bound, each
  watcher's block holding the tasks it watches alone in `watches` order, a restart over a stalled
  console starting before the diagnostic's send times out, a restart waiting for a member
  still sweeping until the DEAD bit, every status record reading after boot over a dirty
  reservation, and the init lowering itself to the table's priority before any other call, 2
  where the composition states none, a refusal panicking.

## 9. What M10.4 uses and what it leaves to M10.5

**Used.** The kernel's Kconfig as the manifest exports it: the pools and budgets,
`KICKOS_CAP_TABLE_SUPPLY`, `KICKOS_MAX_SPAWN_GRANTS`, `KICKOS_MAX_THREAD_WINDOWS`,
`KICKOS_USER_STACK_SIZE` for the default and masked stacks, `KICKOS_MIN_STACK_SIZE`,
`KICKOS_ROOT_STACK_SIZE` for the init, the cores, `KICKOS_RAM_OWNER_SLOTS`,
`KICKOS_ASPACE_RANGES`, and the `init` section's record sizes and free regions. On the
CMake side: `kickos_add_driver` and its catalogue, `cmake/driver_geometry.cmake`, the manifest,
`kickos_emit_image`, the x86_64 link rule, and the gate machinery of
`tests/lib/gate.sh`.

**Left.** `KICKOS_SERVICE_LIST`, `KICKOS_BOARD_PINMAP`, `KICKOS_INIT_PROVIDER`,
`KICKOS_APP_AUTHORITY` and the default init behind the old `kickos` and `kickos_cxx` leaves;
`KICKOS_USER_HEAP_SIZE` sizing those leaves' heap; the service lists' types; the line numbers in
driver descriptors; chip headers generated from chip files; the partition build; x86 linking
through `add_executable`; the out-of-tree examples on `KickOS::system_default`.

## 10. ABI touchpoints

```c
/* user/include/kickos/sys/abi.h */
KOS_SYS_WINDOW_GET,          /* (index, struct kos_window* out) -> 0, or -KOS_EINVAL past the list */
                             /* replaces KOS_SYS_WINDOW_ADDR */
KOS_SYS_TASK_EXIT_STATUS,    /* (task, int* status) -> 0 once ended, or -KOS_EBUSY / -KOS_EBADF / -KOS_EPERM / -KOS_EFAULT */
KOS_SYS_THREAD_SET_PRIORITY, /* (priority) -> 0, or -KOS_EINVAL / -KOS_EPERM */
KOS_TASK_DEAD = 1 << 2,      /* enum kos_task_state: set by task_report_death once every member is swept */
KOS_TASK_ENDED = 1 << 3,     /* enum kos_task_state: set when the entry exits or a member faults */
KOS_SYS_CONSOLE_PUBLISH,     /* now also -KOS_EACCES: the cap lacks HANDOUT; it seats WAIT */
                             /* on a cap without it (2.1) */

/* user/include/kickos/sys.h */
int kos_window_get(uint32_t index, struct kos_window* out);  /* replaces kos_window_addr(base, out) */
int kos_task_exit_status(kos_task_t task, int* status);
int kos_thread_set_priority(uint8_t priority);  /* the caller's own base priority */
/* and the lookups of section 4 */

/* system/include/kickos/sys/atomic.h */
void fence_acquire();        /* std::atomic_thread_fence at acquire, for the status read */
void fence_release();        /* std::atomic_thread_fence at release, for the status write */

/* user/include/kickos/sys/table.h, KICKOS_TABLE_VERSION 3 */
uint8_t init_priority;       /* kos_table_header, after strings_size: what the init lowers itself to */
KOS_GRANT_STATUS = 7,        /* /init/status, a watcher's read-only status block */
uint16_t window;             /* kos_table_grant, after target: the place in the spawn's window list */

/* system/include/kickos/sys/init.h */
void kickos_root_lower(void); /* root's first call: KICKOS_PRIO_ROOT, empty in a system target's init */

/* system/include/kickos/sys/service.h */
struct kos_driver_instance* instance;   /* kos_service_cfg's last field: null on a service list */

/* user/include/kickos/sys/table.h, the reserved fields named, the layout unchanged */
uint32_t block;              /* kos_table_task, after name: a packaged driver's ring block in bytes */
uint16_t flags;              /* kos_table_task, after restart_max: KOS_TABLE_TASK_CONSOLE */
```
