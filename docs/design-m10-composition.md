<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Static composition -- the three files, the table and the init

> **Status: ACTIVE.** This specifies static composition against the golden example in
> [`examples/composition/`](../examples/composition/). `roadmap.md` assigns the M10 work;
> `TODO.md` tracks its remaining implementation items.

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

Hardware facts live under `platform/`, one folder per chip holding its
chip file and boards: independent of architecture, which the i.MX
8M Plus's two clusters need, and readable by the kernel build, the host tool and any tooling
alike. `boards/<board>/` keeps what configures a build of that board -- its defconfigs, its
`board.cmake` and its default composition -- and `arch/` and `kernel/` keep the code. Until M10.3
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
| `heap` | integer | no | the libc heap the image carves |
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
  board's default composition declares `memory`, `system` and `tasks` for `main`.
  A packaged driver's threads take the authority its descriptor states. Its
  `stack`, extra threads and objects come from its
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
type the kernel knows, and `cached` is ordinary memory. Admission checks it against what
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

**A plain app uses a default composition file.** Each
board carries `boards/<board>/composition.yaml`, from which the kernel package builds and exports
a system target, `KickOS::system_default`.

**The user's CMake stays plain:** `add_executable` and
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

Each refusal names the rule it breaks:

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
  them against its reserved blocks and one-holder rule. What the table never
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
| `drivers` | the packaged driver catalogue: roles, threads, objects, ring block, endpoint posture and readiness barrier |

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

Each board's own `Kconfig` is classified by the same rule when its board moves in M10.5.

## Where a window sits

On a translating board the kernel chooses each window's address in the task's own space.
`kos_window_addr` returns that address to the holder; on a region board it returns the
physical base. Tasks cannot assume that a window has its physical address or the same
address in another task. Randomized placement is a later policy, separate from kernel
placement.

Shared memory can be mapped at different addresses in different tasks. Data exchanged
through it therefore uses offsets, not stored pointers. Delegation does not promise to
preserve a reservation's address.

## The lookups

The init passes each task a `kos_self_t const* self` pointing to its table entry. Every
lookup takes `self`, so one entry function can serve several task instances and find
each instance's grants. A task passes `self` to threads it creates.

- `kos_grant_endpoint` and `kos_grant_notify` return `kos_cap_t`.
- `kos_grant_mmio` and `kos_grant_mem` return `kos_window_t`; accessors provide the
  window's address and size. `kos_grant_ports` returns a port-range window whose address
  is its first port. Privileged ports remain outside the task's direct I/O grant and use
  `kos_port_reg_write`, subject to possession, register allowlist and value mask.
- `kos_grant_irq` returns `kos_line_t`, including its capability and index within the
  device. `kos_task_status(self, i, &status)` reports the i-th watched task's name,
  liveness, deaths, restarts left and dependency-down state.

An absent grant returns an invalid handle whose accessors return null, zero or
`KOS_CAP_NONE`. The table and `self` are readable conveniences, not authority: kernel
capabilities and mappings determine what a task can use. A caller substituting another
task's entry gains no access.

## Readiness

A serving task becomes ready on its first wait to receive on the endpoint it serves,
after any packaged-driver readiness barrier and console handover. A task that serves
nothing becomes ready when spawned. The kernel reports the first receive to the
creator's notification, alongside death reports.

The init starts a task only when every endpoint it `uses` is ready. It scans tasks in
file order, skips blocked tasks and rescans them on readiness changes; file order
breaks ties without letting one blocked service stop independent tasks. If a server
exhausts its restart count before becoming ready, dependants remain unstarted with a
dependency-down status, which their watchers can read.

A ready report is a hint about one server instance. The server may die before the init
handles it, and notification bits can merge ready and death. The init therefore handles
deaths first, increments its generation on each start and checks that a pending client's
server is still the live instance that became ready. A restarted server becomes ready
again at its own first receive; callers get `-KOS_EAGAIN` in the gap.

## The init's walk

1. Zero each declared shared region once.
2. Scan tasks in file order. Leave a task pending while an endpoint it `uses` is unready.
   For a ready user task, create its kernel task and served endpoint, retain HANDOUT without
   WAIT, set up watched-task notifications, claim lines, and spawn with its windows, regions
   and capability-bearing grants in declaration order.
3. For a packaged driver, follow its exported `Descriptor`. Create its task, ring block and
   endpoint. If it takes the console, publish it before claiming the UART line, then claim
   and attach its lines, spawn its threads and wait at its readiness barrier. Complete console
   handover by closing the init's receiving right and probing once. Admission requires the
   `stdout` task before tasks that write to it.
4. If resident, wait for notifications. Process deaths before readiness; start newly unblocked
   tasks in file order only when the server instance is still live and ready. After teardown,
   tell watchers, release the dead task's hold and restart it with the same grants while its
   count remains. A console driver repeats handover. Once retries are spent, drop HANDOUT so
   callers get `-KOS_ECONNREFUSED`, and mark its pending dependants as dependency-down.
5. End the system when the declared ending condition occurs.

## Beyond boot: the nested init

A task with the required authority can create tasks and map memory at run time; the
boot init is one user of those kernel calls. It may delegate task-creation authority to
a nested init. A nested init receives readiness and death reports for the tasks it
creates and can use the same restart rules. No kernel or provisioning path singles out
the boot init.

The table's 16-bit indices describe the configured system, while kernel pool limits
remain configurable. A per-spawn grant bound limits one operation, not the lifetime
number of tasks.
