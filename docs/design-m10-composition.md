<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Static composition -- the three files, the table and the init

> **Status: ACTIVE -- settled at M10.0, built from M10.1 on.** Written against the golden example in
> [`examples/composition/`](../examples/composition/), which is the acceptance test and wins
> where the two disagree until this document is settled. `roadmap.md`'s M10 section carries the
> rulings and `TODO.md`'s M10 section the open items; this document states the design they
> add up to, and the questions still open are listed at its end.

## The whole design in one paragraph

A user writes one **composition** per kernel image: the tasks, and for each what it runs, what
it is given and what happens when it dies. A host tool reads it together with the **chip** and
**board** descriptions and the configuration the kernel build exports, refuses it with a message
naming the rule it breaks, or emits one constant **table**. The **init** scans that table in file
order, starts tasks whose declared dependencies are ready, and revisits those still waiting when
readiness changes. It stays resident when the composition says so. A task finds what it was given
by name. No parser runs on the target.

## Who owns what

| file | states | written by | lives |
| --- | --- | --- | --- |
| chip | what the part IS: devices, windows, lines, pins, memory, protection, what the kernel owns | KickOS, once per part | `platform/<chip>/chip.yaml` |
| board | what the board DECIDES: its chip, the console, pin wiring, parts soldered on | KickOS, once per board | `platform/<chip>/<board>.yaml` |
| Kconfig | what the kernel IS: pools, budgets, capability supply, cores, partition | the kernel's integrator | the kernel build, exported |
| composition | what RUNS: tasks, grants, endpoints, shared memory, restart | the user | the user's project |

Hardware facts get a top-level tree of their own, `platform/`, one folder per chip holding its
chip file and its boards (maintainer, 2026-09-28): independent of architecture, which the i.MX
8M Plus's two clusters need, and readable by the kernel build, the host tool and any tooling
alike. `boards/<board>/` keeps what configures a build of that board -- its defconfigs, its
`board.cmake` and its default composition -- and `arch/` and `kernel/` keep the code. Until M10.2
first reads them, the files are the M10.0 draft under `examples/composition/platform/`.

No fact is stated in two of them. The chip file is the source of the kernel's chip headers,
which are generated from it; the composition never restates a kernel figure and may only narrow
what the kernel grants. The partition's assignment of devices to nodes is derived from the
node compositions, not written anywhere.

## Names

A resource is named by a path, and the namespace says who defines it:

| prefix | defined by | example |
| --- | --- | --- |
| `/dev/<device>` and `/dev/<device>/<channel>` | chip file | `/dev/usic0/ch1`, `/dev/rtc` |
| `/dev/<device>/<line>` | chip file | `/dev/usic0/sr1` |
| `/svc/<endpoint>` | composition, `serves` | `/svc/sensor` |
| `/shm/<region>` | composition, `shared` | `/shm/history` |
| `/init/events` | the init, for a task that `watches` | |

A task looks a resource up by the name its entry gives it, and a resource its entry does not
rename is called by its path. User code therefore writes paths. A packaged driver, whose source
cannot know a board's paths, has its entry rename what it needs to the role names its exported
metadata declares, as `lines: { irq: /dev/usic0/sr1 }`.

## The YAML subset, for all three files

- One document per file, and a top-level `version` that the tool knows, or refusal.
- A mapping key the schema does not define is refused, at every level.
- Scalars are typed by the schema, never guessed: booleans are `true` and `false` only, and
  `yes`, `no`, `on` and `off` are refused; an address or a size is an integer, written in hex
  or decimal; a path is a string beginning with `/`.
- No anchors, aliases or merge keys: each file reads top to bottom as it is written.

## The chip file

| field | type | required | meaning |
| --- | --- | --- | --- |
| `version` | integer | yes | schema version |
| `chip` | string | yes | the chip backend's name |
| `arch` | string | unless `cores` gives one per cluster | the kernel architecture |
| `protection` | mapping | unless per cluster | see below |
| `cores` | mapping | no | a core count range and `smp`; on a multi-architecture part one entry per cluster, each with its `arch`, `protection` and `line_offset`, the number its controller adds to a source number |
| `clusters_coherent` | boolean | no | on a multi-architecture part, whether the clusters' caches are coherent with each other; absent is read as `false` |
| `partition_gate` | mapping | no | a bus-enforced assignment of devices to nodes (RDC, APM) |
| `data_cache` | boolean | no | whether a data cache sits over the part's RAM; absent is read as `true` |
| `devices` | mapping | yes | one entry per device, keyed by its name |
| `memory` | mapping | no | ordinary memory windows -- on-chip RAM, flash, apertures -- each a `size` and either a `base` or, where clusters' maps differ, `at`, one base per cluster |
| `pins` | mapping | no | each pin's functions |

`protection` has a `unit` (`pmsav7`, `pmsav8`, `pmsav6`, `pmp`, `rxmpu`, `sysmpu`, `mmu`,
`none`), `covers_devices`, and where they apply `page`, `device_gate` (a gate coarser than a
window: kind, size, whether it is per thread), `bus_gate` (a second unit in series: kind, per
thread, whether a denial traps), `privilege` and `io_ports`. The window encoding rule and the
region budget belong to the unit and come from the kernel build's export, not from this file.

A device entry has a `window` (`[base, size]`), or `ports` for port I/O, or `channels` each with
its own window; `count` and `stride` for a device repeated at a fixed step, instance k being
`/dev/<device>/<k>` at the base plus k times the stride and raising each line plus k; `lines`, a mapping from a line's name to the source number the kernel takes,
listed in the device's order so a line's index within its device is its position, and always a
number (a line private to each core, as the GIC's below 32, is still one); `bus_master`
when it writes memory by physical address; `owner: kernel` when the kernel holds it for life;
`privileged_registers` for registers inside the window only privilege can write; and `cluster`
on a multi-architecture part. Every limitation that makes a grant unenforceable has a name here,
which a refusal quotes and a composition's `accepts` lists.

## The board file

| field | type | required | meaning |
| --- | --- | --- | --- |
| `version` | integer | yes | schema version |
| `board` | string | yes | the board's name |
| `chip` | string | yes | the chip file it builds on |
| `console` | mapping | yes | the kernel console's device and, where wired, its pins |
| `leds` | mapping | no | each LED's pin, active level and owner |
| `parts` | mapping | no | a soldered part: the bus it hangs on, its chip select, its pins |
| `buses` | mapping | no | a bus as this board wires it: device, pins, chip selects |
| `reserved_pins` | mapping | no | pins the board has spent, with the reason |

## The composition

| field | type | required | meaning |
| --- | --- | --- | --- |
| `version` | integer | yes | schema version |
| `board` | string | yes | the board, which names the chip |
| `cluster` | string | on a multi-architecture part | the cluster this image runs on |
| `stdout` | path, or `kernel` | yes | where every task's standard output goes |
| `ends` | `never`, or a task's name | yes | what ends the system |
| `accepts` | list of names | no | platform-wide limitations the composition runs with knowingly |
| `heap` | integer | no | the libc heap the image carves, today's `KICKOS_USER_HEAP_SIZE` |
| `shared` | list | no | shared regions: `name` (a `/shm` path), `size` and `cache` |
| `tasks` | list | yes | in declaration order; ready tasks start in that order |

A task has a `name`; exactly one of `entry` (a symbol in the user's sources) and `driver` (a
driver the package ships); `stack` and `priority`; optionally `core`; `devices`, a list of
device paths, each a register window or a port range, so one task can hold a DMA engine beside
its peripheral; `lines`, a mapping from the names the task looks up to line paths; `serves`, an
endpoint path; `uses`, endpoint paths; `maps`, shared region paths each `ro` or `rw`;
`watches`, task names; `authority`, the kernel authorities it holds; `accepts`, the grant-level
limitations it runs with; and `restart: { max: N }`.

`authority` lists names for the bits of the kernel's authority word: `memory`, `pinmux`,
`pstate`, `irq`, `system`, `console`, and `tasks`, the task-creation authority M10.1 adds. It
is empty unless declared, since authority is never a default; the init passes it as the spawn's
authority word, which can only narrow what the init itself holds, so admission refuses a task
declaring more than the init has. A nested init is the task that declares `tasks`, and the
board's default composition declares `memory`, `system` and `tasks` for `main`, today's fallback
plus task creation. A packaged driver's threads take the authority its descriptor states. The
word is eight bits today and seven would be spent, so M10.1 widens it with the rest of the
kernel share rather than leave the ABI one bit from full. A packaged driver's `stack`, extra threads and objects come from its
exported metadata rather than from the composition.

`accepts` names limitations the host tool **derives** from facts the chip file states -- the chip
file states what the part is, never a verdict -- and each name says where it may be written.

| name | scope | what is accepted | derived from |
| --- | --- | --- | --- |
| `no_protection` | composition | nothing is isolated | `protection.unit: none`, or a build with the unit off (the nRF51, the STM32F103, the F302R8, the LX6) |
| `no_privilege_split` | composition | a task cannot be held unprivileged | `privilege: false` (the nRF51's M0, the LX6, the ESP32-C6's LP core) |
| `device_not_isolated` | task | a device grant the unit cannot see | `covers_devices: false` (the K64F) |
| `coarse_gate` | task | a gate wider than the window, so the neighbours come along | a `device_gate` wider than the window, a whole-port register bank, or two devices in one translating page by `stride` |
| `bus_master` | task | a device that writes memory by physical address | `bus_master: true` (DMA engines, virtio, Ethernet and USB controllers) |
| `cached_incoherent` | composition | a `cached` region shared without coherence | `clusters_coherent: false`, `data_cache`, or a `bus_master` reaching the region |

A platform-wide limitation is accepted once, at the top of the composition; one that belongs to
a grant is accepted on the task holding it, so the risk sits beside the grant that takes it.
Port I/O is not on the list: M10 adds a real grant for it rather than accepting its absence.

A shared region's `cache` is `cached` or `uncached`, and it is required, since whether a
region may sit in a data cache is a decision about who touches it and never a default. It
becomes the region's memory type in the table: `uncached` is `KOS_MEM_NOCACHE`, the one memory
type the kernel knows today, and `cached` is ordinary memory. Admission checks it against what
the region is shared with. Among tasks of one image a `cached` region is sound: they run on
one core, or on a shared kernel's cores, which the multicore contract admits only where memory
is coherent. Where a bus master reaches a region, or where a region is shared across the nodes
of a partition whose chip file does not declare those nodes coherent -- the i.MX 8M Plus's A53
cluster and M7 are not -- `cached` is refused unless the platform limitation is accepted and
the users do their own cache maintenance. And `uncached` is refused where the protection unit
cannot express a memory type and the memory is data-cached; where the part has no data cache
over it, as the ESP32-C6's SRAM has none, both values are equivalent and both admitted.

`ends: never` keeps the init resident until reset. `ends: <task>` ends the system when that
task's entry returns, which is how a plain app's `main` returning shuts the system down.

**A plain app uses a default composition that is a real file** (maintainer, 2026-09-28). Each
board carries `boards/<board>/composition.yaml`, from which the kernel package builds and exports
a system target, `KickOS::system_default`.

**The user's CMake stays plain** (maintainer, 2026-09-28): `add_executable` and
`target_link_libraries(app KickOS::kernel <system>)`. An integrator turns a composition into a
system target with `kickos_compose(<system> <composition.yaml>)` -- the table, the init, the
packaged drivers it names and the link settings -- and hands it over with the kernel; which system
target is linked is the choice of composition. The entry names a composition declares are the
contract between the two, a missing one being a link error naming it. Linking no system target
is a link error too, the kernel referencing a symbol whose name is the message, and linking two
is a duplicate symbol, so no system boots by accident. The checks that need the linked image
become linker-script `ASSERT`s the system target brings, a post-build step being unable to ride a
usage requirement. The file names a packaged entry, `entry: kickos_main`, which calls
`main(argc, argv)`, and declares `ends:` on that task. It is minimal -- the kernel console and
`main` -- so one file fits every preset of its board, and it is the file a user copies to start
composing. A CI gate admits every board's default against that board's manifest.

## What the tool refuses

Each rule refuses with a message naming it, and each has an arm and a mutation proving the arm
turns red. `TODO.md`'s M10.2 item keeps the list current; in outline:

- **Form**: an unknown field, an ambiguous scalar, a missing required field, a version the tool
  does not know.
- **Order**: a `uses` or `watches` naming something not declared above it, so references point
  backward and no dependency graph is solved; an `ends` naming no task.
- **Ownership**: a window, a line or a gate around them held twice; a grant overlapping a
  kernel-owned block; a device two nodes of a partition grant.
- **Encoding**: a window the protection unit cannot encode; more windows for a task than its
  region budget; on a translating board a device window that is not whole pages, or a page
  holding two devices, unless accepted; a shared region rounds up to the granule instead.
- **Enforcement**: a grant the platform cannot enforce -- a bus-side unit, a gate coarser than
  the window, a bus master, no unit or no privilege split -- unless its named limitation is in
  `accepts`; a port range held twice or overlapping a kernel-owned one, as a window is.
- **Supply**: counts against the kernel's pools, per-task budgets and capability supply, each
  refusal naming the Kconfig knob that would have to grow; a task's capability-bearing grants,
  all delegated at its spawn, against `KICKOS_MAX_SPAWN_GRANTS`; stacks and shared regions against the
  arena and the link.
- **Scheduling**: a declared `core` the kernel build lacks; a task taking a line not on exactly
  one core; a stdout writer above the console's priority; a task that writes standard output
  declared before the task `stdout` names.
- **Memory type**: a shared region's `cache` against who shares it, as the composition section
  states.

## The emitted table

One constant table per image, in memory every task can read. It carries no authority: every
capability it mentions is already in the kernel, and reading another task's entry reveals
addresses, not access.

Every index is 16 bits and every count names the Kconfig knob that bounds it. Sixteen bits is a
ceiling of the format, 65,535 entries of a kind, chosen to cover every configuration the kernel
supports; a larger one would need a new table version, which the header's version field is there
to carry. A string is an offset into the pool.

```text
header    magic u32, version u16, flags u16 (ends: never / on a task),
          ends_task u16, task_count u16, grant_count u16, ref_count u16, priv_count u16,
          region_count u16, strings_size u32 -- the arrays follow in this order: task[],
          grant[], ref[], priv[], region[], then the strings

task      name u32, entry (a pointer: user code, or a packaged driver's start),
          driver u16 (catalogue index, or none), stack u32, priority u8, restart_max u8,
          core_mask u32, authority u32 (the word the spawn seats),
          first_grant u16, grant_count u16, cap_grant_count u16,
          first_use u16, use_count u16      -> ref[]: the tasks whose endpoints it uses
          first_watch u16, watch_count u16  -> ref[]: the tasks it watches

grant     kind u8 (endpoint_serve, endpoint_use, notification, window, ports, region, line),
          flags u8 (ro, uncached), cap_slot u16 (the ordinal of a capability-bearing grant,
          or none), name u32 (what the task looks it up by), path u32,
          target u16 (the served endpoint's task, or the region),
          base u64 (a window's PHYSICAL base, or a port range's first port),
          size u32 (bytes, or ports), line_index u16, line u16 (the source number the kernel
          takes), priv_first u16, priv_count u16 -> priv[]: the privileged registers inside it

ref       task u16
priv      offset u16, width u8 (1, 2 or 4 bytes): left out of the task's direct reach; the
          bits a write may carry are kernel policy and stay in the kernel's allowlist
region    name u32, size u32, flags u8 (uncached)
strings   the names, NUL-terminated
```

`cap_grant_count` is what admission holds within `KICKOS_MAX_SPAWN_GRANTS`; `use` and `watch`
are what readiness and death reports follow. A generation per task, which the readiness rule
needs, is run-time state the init keeps, not part of the constant table.

A task's grants are contiguous and in declaration order, and they are of two sorts. A grant
that carries a capability -- an endpoint served or used, a notification, a line -- is delegated
at spawn, and those grants alone are numbered: the i-th of them lands at slot
`KOS_SPAWN_DELEGATED_CAP0 + i`, the slot the table records. A window or a shared region is a
mapping the spawn makes, occupies no capability slot, and records none.

**The table carries every device fact the init needs, so no kernel device catalogue is
required.** A window grant states its physical base and size, a port grant its first port and
count, and the kind says which: the init hands exactly those to the spawn, and the kernel checks
them against its reserved blocks and one-holder rule as it does today. What the table never
carries is the address a translating board maps a window at, which the kernel chooses and
`kos_window_addr` reads back: see the next section. A physical base is a chip fact, public in the
chip file, and granting nothing.

## The export manifest

What the installed kernel package carries so that a composition is checked without the kernel's
source tree: one YAML file, same subset, generated by the kernel build and installed beside the
package's CMake files, where `kickos_compose` finds it.

| section | contents |
| --- | --- |
| `abi` | the version of the table layout and of the lookups the kernel and its init accept |
| `target` | board, chip, arch; kernel cores, isolated cores; the AMP node, its peers and ports |
| `protection` | the unit; its window rule (power of two naturally aligned, or a granule multiple), smallest window and granule; the regions a task has left for grants after its code, data and stack; the page on a translating board; the memory types it honours |
| `pools` | every `KICKOS_MAX_*`, every `KICKOS_TASK_*_BUDGET`, `KICKOS_MAX_SPAWN_GRANTS`, `KICKOS_CAP_TABLE_SUPPLY` |
| `threads` | the priority range, the policies, `KICKOS_MIN_STACK_SIZE`, the stack a runtime spawn defaults to |
| `memory` | on a region board the user arena's base and size; the image's own carve |
| `descriptions` | the chip and board files the kernel was built from |
| `default` | the board's default composition file, and the `KickOS::system_default` target built from it |
| `drivers` | the packaged driver catalogue: each driver's roles (window, lines and their triggers), threads with their priority offsets and stacks, the objects it creates, its ring block, its endpoint posture (handover or retain) and its readiness barrier -- today's `Descriptor`, exported |

Two things are known only once the user's image is linked -- its thread-local block and its data
carve -- so admission runs twice: the composition against the manifest when the system target is
built, and the image's layout against the arena at the link, as linker-script `ASSERT`s the
system target brings. Both refuse with a message naming the rule.

## The Kconfig split

Classified against the rule that Kconfig configures the kernel and the composition configures
userspace:

- **Moves to the composition or is deleted, being userspace.** `KICKOS_SERVICE_LIST` and
  `KICKOS_BOARD_PINMAP` are deleted: services are tasks and pins are the board file's.
  `KICKOS_USER_HEAP_SIZE`, the libc heap the image carves, becomes an image-level `heap` in the
  composition. The `KICKOS_INIT_PROVIDER` CMake cache entry is deleted: there is one init.
- **Stays, being the kernel.** Cores and the multicore model; the whole AMP partition, ports
  included; the memory model and paging mode; every pool, budget, spawn-grant bound and
  capability supply; the kernel's own stacks, idle's and `KICKOS_MIN_STACK_SIZE`; the kernel
  console and telemetry transports; the debug, self-test, bench and diagnostic knobs; the
  multi-instance sim knobs.
- **Stays, with a different reason than a first reading suggests.** `KICKOS_USER_STACK_SIZE` is
  the stack the kernel gives a runtime spawn that brings none, which a nested init relies on;
  a composition declares its own tasks' stacks and never restates it. `KICKOS_ROOT_STACK_SIZE`
  is the init's, and the init is KickOS's code.
- **Stays as a selection, its facts moving to the chip file.** The board, chip and arch
  selectors choose what the kernel is built for; what the chip is comes from its file. The GIC
  version is a fact on silicon and a machine option on QEMU `virt`, so it stays a knob there.
- **Derived, unchanged.** The knobs without a prompt, computed from the others.

Each board's own `Kconfig` is classified by the same rule when its board moves in M10.4.

## Where a window sits

**On a translating board the kernel chooses, when it maps the window** (maintainer,
2026-09-28), as the kernels of general-purpose systems do. Each task is a process: its address
space is its own, and nothing ties a window's address in it to the physical address or to the
address another task sees. A static one-to-one mapping was refused for being easier to attack --
a network stack whose device and buffer addresses are known in advance hands an attacker who
found a bug in it half the work -- and for breaking that notion of a process. So
`kos_window_addr` asks the kernel, which answers from the mapping it made; on a region board the
same call answers the physical base, a region board having no other address to give. One path,
both classes.

**Choosing and randomizing are separate decisions.** M10 makes the kernel the one that chooses.
Whether the choice is randomized is a later policy for translating boards only, and one worth
having; it pays fully only once task code and stacks stop sitting at fixed link addresses, which
is position-independent task images, a larger topic than M10. Region boards run one-to-one by
nature and are out of its scope.

**Shared memory is at a different address in each task that maps it.** A pointer stored in a
shared region means nothing to another task, so what tasks exchange there is offsets.
`kos_ram_alloc`'s comment says delegation preserves a reservation's address; that describes
today's behaviour and is not an invariant (maintainer, 2026-09-28), so the comment changes with
the translating arm.

## The lookups

Every task entry receives its own entry in the table as its first argument, `kos_self_t const*
self`, as a program's first argument names the program (maintainer, 2026-09-28), and every
lookup takes it. So one entry function can run as several tasks -- two instances of one UART
driver -- and each finds its own grants; a thread the task creates is handed `self` like any
other argument. No kernel change is needed: the init passes it through the argument every
thread entry already has.

`kos_grant_endpoint(self, name)` and `kos_grant_notify(self, name)` answer a `kos_cap_t`: a
capability already is an opaque handle. `kos_grant_mmio(self, name)` and `kos_grant_mem(self,
name)` answer a `kos_window_t`, read through `kos_window_addr` and `kos_window_size`, and
`kos_grant_ports(self, name)` answers a `kos_window_t` too, whose address is the range's first
port. A privileged register inside a port range, as the CMOS clock's index is, is left out of
the task's I/O bitmap and written through `kos_port_reg_write(base, offset, value)`, a
byte-wide write M10.1 adds beside `kos_periph_reg_write`, whose store is one aligned 32-bit word
in a memory window and so cannot serve a one-byte port. Its possession check is the port
counterpart of the memory one: the caller holds a port grant covering `base + offset`, and the
register is on the kernel's allowlist with the value inside its mask -- the CMOS index's mask
withholding bit 7, the NMI mask. `kos_grant_irq(self, name)` a `kos_line_t`, read through its capability and its index within its
device. A task that `watches` others calls `kos_task_status(self, i, &status)` for the i-th of
them: alive, deaths, restarts left, its name, and whether it was never started because a
dependency is down. A handle names one of the task's grants, so nothing is allocated, and a name the task
was not given answers the invalid handle, whose accessors answer null, zero or `KOS_CAP_NONE`.
**Neither `self` nor the table enforces anything.** Both are readable, and a task that passes
another task's entry, or reads the table directly, learns only names, sizes and slot numbers:
what a task can actually reach is decided by the capabilities in its own kernel table and the
mappings the kernel made for it, so a forged lookup finds a slot that holds something else or
an address that faults. The lookup is a convenience over authority the kernel already holds.
Because the address is read through an accessor, where a window sits in a task's address space
can change without touching a line of user code.

## Readiness

**File order makes a server exist before its clients; readiness makes it able to answer.** A
client started after its server's spawn never finds the endpoint unserved -- the server holds its
receiving right from the spawn, so an early call parks until the server takes it. What file order
does not give is a server that has finished its own bring-up, and a client that treats a failed
first call as fatal would then spend its own restart count on its server's: `sensor_spi.cc`
returns when its bus open fails, and a bus service dying during bring-up would cost the sensor a
restart each time.

**A serving task is ready the first time it waits to receive on the endpoint it serves**, which
is exactly when it can take a request, so no user call is needed and none can be forgotten. A
packaged driver is ready at the same moment, after its descriptor's own readiness barrier and,
for a console, the handover. A task that serves nothing is ready once spawned. The kernel reports
a first receive on its creator's notification, beside deaths -- one event source, and a small kernel
change of the same kind as the death report.

**The init starts a task only once every endpoint it `uses` is ready, and skips past one that
must wait** (maintainer, 2026-09-28, after systemd's job engine). The dependencies are the ones
the composition already declares and file order already puts first, so nothing is solved. The
init walks the tasks in file order; one whose dependencies are not all ready stays pending and
the walk goes on, and each ready event rescans the pending tasks in file order and starts those
now satisfied. So file order is the tie-break, never a wall: a stuck `spi0` holds back the sensor
that uses it and not the `health` task that is there to report it.
**A task that can never start is reported, not left silent.** When a server's restart count is
spent before it was ever ready, the tasks waiting on it cannot start; `kos_task_status` answers
them as not started with a dependency down, and their watchers are told, as systemd marks a unit
whose dependency failed.

**A ready event belongs to one instance of the server, and it is a hint.** A server can reach its
first receive and die before the init handles the event, and a notification's bits merge a death
and a ready into one wake. So on each wake the init handles deaths before readiness, each task
carries a generation the init increments on every start, and before starting a pending client
the init confirms that the server's live instance -- the current generation -- is the one that
became ready. A ready event from an instance that has since died is discarded (found by the
external audit). The same rule holds after a restart:
a restarted server is ready again at its first receive, and in the gap its callers answer
`-KOS_EAGAIN`, whose contract is that retrying may succeed.

## The init's walk

1. Zero each declared shared region once.
2. For each task in file order: if an endpoint it `uses` is not ready, leave it pending and go
   on to the next. Otherwise, a user task: create its kernel task; create the endpoint it
   `serves`, keeping the right to hand out receiving without being a receiver; create the
   notification for a task that `watches`, keeping one badged copy per watched task; claim its
   lines; spawn its thread with its windows and regions mapped and its capability-bearing
   grants delegated in order; start it.
   A packaged driver runs the sequence its exported descriptor states -- the same `Descriptor`
   `user/include/kickos/sys/driver_service.h` defines today -- with its window and lines taken
   from the table instead of a service configuration: create its task and any ring block;
   create its endpoint; **publish it first if the driver takes the console over**, because until
   the publish the kernel's own console handler is attached to the UART's line and a claim of
   that line is refused; claim its lines, pinned to the core they are claimed on; attach them to
   its notification; spawn its threads, holding at the descriptor's barrier until the driver
   latches ready; and for a console, finish the handover -- close the init's own receiving right
   so the driver is the only receiver, then probe the console once. A task spawned after the
   publish finds its standard output on the new console, which is why admission requires the
   `stdout` task to be declared before every task that writes to it.
3. If the composition keeps it resident, wait on the init's notification. Handle deaths first;
   then, for each ready event whose server is still the instance that became ready, start the
   pending tasks, in file order, whose dependencies are now all ready. On a death, let the
   kernel's teardown finish, raise the watchers' bits, and restart the task with the same
   grants while its count lasts -- a packaged driver by running its descriptor's sequence again,
   which for a console repeats the publish, the kernel having taken the console back when its
   only receiver died. Once the count is spent, drop the right to hand out its endpoint's
   receiving, so its callers answer `-KOS_ECONNREFUSED` from then on, and report each task still
   pending on it as not started with a dependency down.
4. When the declared ending condition is met, end the system.

## Beyond boot: the nested init

The composition describes the system at boot, not its whole life (maintainer, 2026-09-28:
general-purpose use must stay possible, and M10 may not block it). A task the composition starts
may itself be an init for a dynamic subsystem: a session manager that launches programs at run
time -- loaded from storage, started with capabilities it holds and chooses to hand on -- the way
a desktop is built on a static, capability-based base. So the boot init is not special in kind:

- M10 preserves the existing ability of a task with the required rights to create tasks and
  map memory at run time. The boot init is their first user, not their only possible user; no
  new path in the kernel or the provisioning library tests for "the init". Task creation is
  creator-scoped today, while a memory grant requires memory authority. M10 closes with a
  separate task-creation authority, built in M10.1 with the kernel share, which the composition grants: the boot
  init holds it and hands it to a nested init, and a task without it cannot create tasks;
- a nested init uses the same restart, readiness and death machinery for its own children, so
  the death and readiness reports are raised to whoever created the task, not to the boot init
  by name;
- runtime objects are mapped at run time through the calls that already exist, as ruled above;
- the table's indices represent the supported configured system size, and M10 adds no fixed
  system-wide ABI ceiling. Pool limits remain configurable; a per-operation bound such as the
  spawn grant count may stay small without limiting how many tasks a system can create.

## Open questions

- **More than one device window per task: M10 lifts the limit** (maintainer, 2026-09-28). A
  spawn carries one window today (`mmio_base`, `mmio_size`), and the kernel records exactly one
  per thread (`Thread::dev_base`, `dev_size`), which both the one-holder check and the gate on
  `kos_periph_enable` and `kos_periph_reg_write` read. So a task cannot drive a DMA engine and
  its peripheral, or a few devices directly without a server. The kernel change: the spawn
  carries a window list, the thread keeps a small array bounded by the protection unit's region
  budget, the one-holder check runs per window, and the peripheral seams accept any window the
  caller holds. Admission checks the budget offline, as it already does for the region count.
- **The kernel changes restart and readiness need**: a task's death and an endpoint's first
  receive raised on the creating task's notification -- the boot init's for the tasks it starts,
  a nested init's for its own -- and the right to hand out an endpoint's receiving
  without counting as a receiver.
- **Settled at M10.0's close**: the `accepts` names and their scope, the table layout, the export
  manifest and the Kconfig split, approved by the maintainer.
- **Settled since the draft**: a plain app uses the default composition file the export ships
  for its board, below.
- **Built in M10.1, the kernel share**: the kernel mechanisms listed above, and the port grant: a
  per-task I/O permission bitmap loaded into the core's task-state segment on each switch.
