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
`board.cmake` and its default composition -- and `arch/` and `kernel/` keep the code.

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
| `/amp/<port>` | the partition's port list (`KICKOS_AMP_PORTS`), a crossing between nodes | `/amp/3` |
| `/init/events` | the init, for a task that `watches` | |
| `/init/status` | the init, for a task that `watches`: the status of the tasks it watches, in `watches` order, read-only | |

Instance k of a device repeated by `count` is `/dev/<device>/<k>`, and its line is
`/dev/<device>/<k>/<line>`, the device's line plus k: `/dev/virtio/3/irq` is source 51.

A task looks a resource up by the name its entry gives it, and a resource its entry does not
rename is called by its path. User code therefore writes paths. A packaged driver, whose source
cannot know a board's paths, has its entry rename what it needs to the role names its exported
metadata declares, as `lines: { irq: /dev/usic0/sr1 }`.

## The YAML subset, for all three files

- One document per file, and a top-level `version` that the tool knows for that file's format,
  each format keeping its own list, or refusal.
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
| `manual` | string | yes | the document the values come from, with its version, which opens every header generated from the file |
| `arch` | string | unless `cores` gives one per cluster | the kernel architecture |
| `protection` | mapping | unless per cluster | see below |
| `cores` | mapping | no | a core count range and `smp`; on a part whose cores are protected differently, one entry per cluster, each with its `protection`, and on a multi-architecture part also its `arch`, count range, `smp` and `line_offset`, the number its controller adds to a source number |
| `clusters_coherent` | boolean | no | on a multi-architecture part, whether the clusters' caches are coherent with each other; absent is read as `false` |
| `partition_gate` | mapping | no | a bus-enforced assignment of devices to nodes: its `kind` (`apm`, `accessctrl`, `rdc`), and either the `device` it is programmed through, a per-peripheral gate each device names its register in by `gate_register` with the registers no node is assigned `never_assigned` (each a name and its offset), or its `gates`, each a device of the chip with the `regions` it holds (its catch-all among them), the device `ranges` it fronts, the `memory` it fronts for every other node, and the ranges every other node's `kernel` holds through it, each a `window` and its `access` |
| `data_cache` | boolean | no | whether a data cache sits over the part's RAM; absent is read as `true` |
| `devices` | mapping | yes | one entry per device, keyed by its name |
| `memory` | mapping | on a part with a core that does not translate | ordinary memory windows -- on-chip RAM, flash, apertures -- each a `size` and either a `base` or, where clusters' maps differ, `at`, one base per cluster, or neither for the arena of a part the host runs, which the host places and the link has no region for, and `cluster` on an entry one cluster alone reaches; for the part, or each of its clusters, that does not translate, exactly one entry it reaches is marked `arena: true`, the RAM its user arena is carved from |
| `pins` | mapping | no | each pin's functions |
| `interrupts` | mapping | yes | `count`, the controller's line count; `soft_only_from`, the first line no hardware raises; `free_from`, the first line no device uses; `vectors`, the RX's INTB table size; each a number or `{ value: <number>, ref: <ref> }`. On a multi-architecture part the lines are source numbers, and the generator adds the built cluster's `line_offset` to each; every device's line is below `count` |
| `cycle_counter` | mapping | no | `hz: 0` where the counter has no fixed rate, `glitches: true` where a read can glitch, each a value or `{ value: <value>, ref: <ref> }` |
| `reserved` | `none` | no | stated by a part whose protecting unit holds no window for the kernel, which the tool otherwise refuses: a protecting part marks the devices the kernel holds `owner: kernel` |
| `c` | mapping | no | `namespace`, the C++ namespace of the generated headers where it is not `kickos::<chip>`, `line_enum`, the line enum's declaration where it is not `irq_num`, as `node : int`, and `led: addressable` where the chip's code drives the kernel's LED as one sent its state as data rather than a `level` one |

`protection` has a `unit` (`pmsav7`, `pmsav8`, `pmsav6`, `pmp`, `rxmpu`, `sysmpu`, `mprotect`,
`mmu`, `none`), `covers_devices` and `memory_type` unless the unit is `none`, `page` on `mmu` and only
there, and where they apply `device_gate` (a gate coarser than a
window: kind, size, whether it is per thread), `bus_gate` (a second unit in series: kind, per
thread, whether a denial traps), `privilege`, `io_ports`, and `driven: false` on a region unit no
kernel build drives, which no build then enforces and Kconfig selects no `HAS_MPU` for. A
`device_gate` may list the
`ranges`, each `[base, size]`, that its gate fronts, and without them it gates every device
window; a device window a range holds only in part is refused. The window encoding rule and the
region budget belong to the unit and come from the kernel build's export, not from this file.
`memory_type` states whether the unit's descriptor or page-table entry carries a memory type, a
fact of the hardware. With `data_cache` and whether the build enforces its unit it decides
`uncached`: refused where a data cache sits over the memory and the entry carries no type,
programmed where it carries one, and already met where no data cache reaches grantable memory or
the build enforces nothing, although the rv64 kernel admits `KOS_MEM_NOCACHE` on QEMU `virt`,
whose entries carry none, without honouring it (`TODO.md`).

A device entry has a `window` (`[base, size]`), or `ports` for port I/O, or `channels` each with
its own window; `count` and `stride` for a device repeated at a fixed step, instance k being
`/dev/<device>/<k>` at the base plus k times the stride and raising each line plus k; `lines`, a mapping from a line's name to the source number the kernel takes,
listed in the device's order so a line's index within its device is its position, and always a
number (a line private to each core, as the GIC's below 32, is still one); `bus_master`
when it writes memory by physical address; `owner: kernel` when the kernel holds it for life, in
which case its `window` or `ports` is required, so its reserved range is checked from this file,
and a device reached through system registers alone, as the generic timer is, says so with
`sysreg: true` instead; `host: true` for a device the host reaches for it, with no window, which
only a part the host runs has, its unit being `mprotect`; `privileged_registers` for registers
inside the window only privilege can write; and `cluster` on a multi-architecture part. Every limitation that makes a grant
unenforceable has a name here, which a refusal quotes and a composition's `accepts` lists.

Any device, channel, line, block or memory entry may carry `ref`, where in the manual its value
is, which reaches the generated header as a comment beside its symbol, and `symbol`, its C name
where today's differs from the derived one: a device's base `<DEVICE>_BASE`, a channel's
`<DEVICE>_<CHANNEL>_BASE`, a block's `<DEVICE>_<BLOCK>_BASE`, a repeated device's first instance
`<DEVICE>0_BASE` with `<DEVICE>_STRIDE`, a memory entry's `<ENTRY>_BASE`, and a line
`<DEVICE>_<LINE>`; two entries one C name names are refused. A `ref` or `manual` is printable
ASCII with no `*/` and no backslash, since a C comment carries it, and a window or memory entry
ends within the addresses its view's architecture reaches. A line or a block carrying either is
a mapping, `{ number: <line> }` or `{ offset: <offset> }` beside them. A device's `blocks` names
sub-blocks of its one `window`, each an offset inside it. A memory entry's `link` states the
linker region it is and its access, `{ region: FLASH, access: rx }`, one entry per region in each
view.

A pin entry maps each selector to the function it selects, as `<device>.<signal>`, and plain GPIO
is the function whose selector is `gpio`, naming the device that holds the pin's port registers as
`<device>.<k>.<bit>` for instance k of a repeated device and `<device>.<bit>` otherwise. Every pin
a board file names is a pin of its chip, and a console pin, an LED pin, a reserved pin or a chip
select has a `gpio` function.
A console pin's selector names the value its mux field takes, which reaches `board_pins.h`: the
number it ends in (`af7`, `f2`, `psel11`, `remap0`), or, for a one-letter selector, its place from
`a` = 0. A selector of more letters and no number, as `in`, names no value. A function may be a mapping,
`{ function: <device>.<signal>, input_select: <value>, ref: <text> }`, where the device has an
input-select register whose value picks this pin, as the i.MX RT's DAISY does; a `gpio`
function is not.

## The board file

| field | type | required | meaning |
| --- | --- | --- | --- |
| `version` | integer | yes | schema version |
| `board` | string | yes | the board's name |
| `chip` | string | yes | the chip file it builds on |
| `console` | mapping | yes | the kernel console's device and, where wired, its pins, each role the device's signal its pin carries (`tx`, or the USIC's `dout0`), or `semihosting: true` where the console is semihosting and so no device |
| `leds` | mapping | no | each LED's pin, `kind` and owner, at most one being `owner: kernel`, the LED the kernel drives, whose `kind` is the chip file's `c` `led`; `kind: level`, the default, states the `active` level that lights it, and `kind: addressable`, an LED sent its state as data, states none |
| `parts` | mapping | no | a soldered part: the bus it hangs on, its chip select, its pins |
| `buses` | mapping | no | a bus as this board wires it: device, pins, chip selects |
| `reserved_pins` | mapping | no | pins the board has spent, with the reason |
| `memory` | mapping | no | memory soldered on the board, external flash, PSRAM or DRAM: each a `size`, a `base` in its chip's map, `cluster` where one cluster alone reaches it, and the `link` region the image is placed in, as a chip file's memory entry states them; it may overlap no window of its chip, link a region its chip already links, or take a C name its chip's headers already carry, and it joins the chip's in the headers generated for the board |

A board spends each pin once: a pin named twice across the console, the LEDs, the parts and the
buses is refused, as one also among `reserved_pins` is, but for a bus line two parts on one bus
both list.

## The composition

| field | type | required | meaning |
| --- | --- | --- | --- |
| `version` | integer | yes | schema version |
| `board` | string | no | the board, which names the chip; absent, the board of the kernel build the composition is admitted against, so one file fits every board whose files define what it names, and a name the build's board lacks is refused as it is under a named board; a board's default composition names it |
| `cluster` | string | on a multi-architecture part, unless `board` is absent | the cluster this image runs on |
| `stdout` | path, or `kernel` | yes | where every task's standard output goes |
| `ends` | `never`, or a task's name | yes | what ends the system |
| `accepts` | list of names | no | platform-wide limitations the composition runs with knowingly |
| `heap` | integer | unless `board` is absent | the image's libc heap, in bytes, which the link carries and no kernel figure sets; `heap: 0` carves none |
| `init` | mapping | no | the init's own: `priority`, which the init lowers itself to at boot, one above the kernel build's lowest when absent |
| `shared` | list | no | shared regions: `name` (a `/shm` path), a nonzero `size` and `cache`, and `partition: true` for a partition region, which lives in the partition's user share and every node naming it maps |
| `tasks` | list | yes | in declaration order; ready tasks start in that order |

A composition naming no board takes what it leaves out of `cluster`, `heap` and `accepts`, and an
`entry` task's `stack`, from the default composition of the build's board, the stack being that
of the default's own `entry` task. What it states is admitted as it is under a named board, an
empty `accepts` included, so the one file states only what its app adds to every board's default.
An `entry` task's `ceiling` is never taken from the default: every such task states its own.

A task has a `name`; exactly one of `entry` (a symbol in the user's sources) and `driver` (a
driver the package ships); `stack` and `priority`; on an `entry` task `ceiling`, the highest
priority the task's threads may take, at least its `priority` and within the kernel build's range,
a packaged driver's being derived instead; optionally `core`; `devices`, a list of
device paths, each a register window or a port range, so one task can hold a DMA engine beside
its peripheral; `lines`, a mapping from the names the task looks up to line paths; `serves`, an
endpoint path or a crossing `/amp/<port>` the partition names this node's; `uses`, endpoint paths
or crossings; `maps`, shared region paths each `ro` or `rw`;
`watches`, task names; `authority`, the kernel authorities it holds; `accepts`, the grant-level
limitations it runs with; and `restart: { max: N }`. A task name, a line name, a role name and a
driver name are lowercase identifiers, `[a-z][a-z0-9_]*`; `core` is below 32, the table's
`core_mask` being 32 bits, and every other integer is bounded by the width of the table field that
carries it.

`authority` lists names for the bits of the kernel's authority word: `memory`, `pinmux`,
`pstate`, `irq`, `system`, `console`, and `tasks`, the task-creation authority M10.1 adds. It
is empty unless declared, since authority is never a default; the init passes it as the spawn's
authority word, which can only narrow what the init itself holds, so admission refuses a task
declaring more than the init has. The boot init holds every authority, so against it a name has
only to be one the tool knows; a nested init holds what its creator gave it, and what it may grant
is checked against that. A nested init is the task that declares `tasks`, and the
  board's default composition declares `memory`, `system` and `tasks` for `main`.
  A packaged driver's threads take the authority its descriptor states. Its
  `stack`, extra threads and objects come from its
exported metadata rather than from the composition.

A packaged driver's metadata is declared where the driver is built, on `kickos_add_driver`: the
role names of its windows and lines, its threads with their priority offset, stack, the
capabilities each one's spawn delegates and the badged copies of the driver's notification among
them, the thread that receives on its endpoint, the
endpoints and notifications it creates, its ring block, a power of two, and the block's memory
type, `BLOCK_CACHE cached` or `uncached`, its endpoint posture,
its readiness barrier, whether it takes the console, `START`, the C function the init calls
to bring it up, at which a driver task's `entry` points, and `CLIENT`, the libraries a task using
it links, each a target the manifest's export refuses the driver for lacking. The build emits it
twice, into a generated header the driver's `Descriptor` reads and into the manifest's catalogue,
since a value the build reads is never read back out of C. A driver task's `devices` bind in
order to its window roles, as many as it declares, and its `lines` name exactly its line roles.

`stdout` names the endpoint served by a packaged driver that takes the console, or `kernel`. The
board's console device is granted to that task alone, and to none while `stdout` is `kernel`.

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
a grant is accepted on the task holding it, so the risk sits beside the grant that takes it. A
name nothing in the composition needs is refused, so a stale acceptance cannot hide a change.
Need is judged across every build of the part from the chip and board files alone, so one file
fits every build of its board: a name only some builds need, as `no_protection` is needed by a
build with a region unit off, is admitted when listed against a build that enforces it, and a
name the files rule out is refused, as `device_not_isolated` is where the unit covers devices and
`no_protection` where every unit translates, a translating unit never being built off. On
a part whose clusters are protected differently and share one `arch`, as the ESP32-C6's are, a
composition names no cluster, and the limitations derived are the union of every cluster's.
Under `no_protection` no grant is enforced, so no grant-level limitation is demanded; listing
one is refused as unneeded where no build of the part enforces a grant, and admitted where a
build that enforces would demand it. A device that a pin's `gpio` function names holds that pin, so
granting it to a task while the board spends one of its pins elsewhere (console, LEDs, parts,
buses or reserved pins) needs `coarse_gate` on that task.
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
task's entry returns or calls `exit`, or the task faults, which is how a plain app's `main`
returning shuts the system down.

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
`main(argc, argv)`, runs `main` at priority 2 under `ceiling: 31`, so `main` may spawn threads
above itself, and declares `ends:` on that task. It is minimal, the kernel console and `main`,
so one file fits every preset of its board, and it is the file a user copies to start
composing. A CI gate admits every board's default against that board's manifest.

## What the tool refuses

Each refusal names the rule it breaks, the rules of each class listed after it:

- **Form**: an unknown field, an ambiguous scalar, a missing required field, a version the tool
  does not know. `form.*`.
- **Order**: a `uses` or `watches` naming something not declared above it, so references point
  backward and no dependency graph is solved; an `ends` naming no task. `order.undeclared`,
  `order.forward`.
- **Restart**: `restart` on the task `ends` names, whose one exit ends the system, and on a
  packaged driver of a kernel build without fault isolation, the manifest's `protection` stating
  which, where a failing driver thread's trap ends the system. `restart.ends`,
  `restart.no-isolation`.
- **Ownership**: a window, a line or a gate around them held twice; a kernel-owned device named
  in a grant; the board's console device granted to a task `stdout` does not name. Within a chip
  file, no window or port range overlaps another. `ownership.device`, `ownership.line`,
  `ownership.gate`, `ownership.kernel`, `ownership.console`, `chip.overlap`.
- **Encoding**: a table holding more than the 65,535 entries of a kind a 16-bit index reaches,
  or a value that reads as none where none may stand; a window the unit cannot encode and more
  windows for a task than `KICKOS_MAX_THREAD_WINDOWS`, its region budget, a watcher's
  `/init/status` among them, both of which the spawn checks whether or not the build enforces its
  unit; a task watching more than the 32 tasks the bits of its notification stand for; on a
  translating board a device window that is not whole pages, unless its page holds another
  device's window as well and the task accepts `coarse_gate`, a device alone in its page being
  described at page size in the chip file; a shared region rounds up to the granule instead.
  `encoding.table`, `encoding.window`, `encoding.budget`, `encoding.watches`, `encoding.page`,
  `encoding.page-shared`.
- **Enforcement**: a grant the platform cannot enforce -- a bus-side unit, a gate coarser than
  the window, a bus master, no unit or no privilege split -- unless its named limitation is in
  `accepts`; a port range held twice or overlapping a kernel-owned one, as a window is.
  `enforcement.*`.
- **Supply**: counts against the kernel's pools, per-task budgets and capability supply, each
  refusal naming the Kconfig knob that would have to grow; a task's capability-bearing grants,
  all delegated at its spawn, against `KICKOS_MAX_SPAWN_GRANTS`; on a region board the init's
  self-grants against what root's region set holds past its static regions and its stack, and
  its reservations against `KICKOS_RAM_OWNER_SLOTS` where the build enforces its unit; on a
  translating board each address space's range records, the init's and each task's, against
  `KICKOS_ASPACE_RANGES`; on a region board, the init's blocks, shared regions, ring blocks and
  stacks against the RAM the arena is carved from, the link checking the arena itself.
  `supply.pool`, `supply.budget`, `supply.cap-table`, `supply.spawn-grants`, `supply.stack`,
  `supply.init-windows`, `supply.reservations`, `supply.ranges`, `supply.arena`, `supply.size`.
- **Scheduling**: a task, or a packaged driver's thread at its priority plus its offset, outside
  the kernel build's priority range; a `ceiling` below its task's `priority` or outside that
  range, and one on a packaged driver, whose ceiling is its priority plus its threads' highest
  offset; a declared `core` the kernel build lacks; a task taking a
  line not on exactly one core; `stdout` naming an endpoint no console driver serves, or a console
  driver whose endpoint `stdout` does not name; a stdout writer above the console's priority,
  which is that of the driver thread receiving on its endpoint; a task that writes standard
  output declared before the task `stdout` names; the init's priority, stated or defaulted,
  outside the kernel build's range. A thread at or above the init's priority that never blocks starves the init,
  which is the user's choice: the init then handles no death and does not end the system.
  `scheduling.priority`, `scheduling.ceiling`, `scheduling.core`, `scheduling.line-core`, `scheduling.console-driver`,
  `scheduling.stdout-priority`, `scheduling.stdout-order`, `scheduling.init-priority-range`.
- **Memory type**: a shared region's `cache` against who shares it, as the composition section
  states. `memory.uncached`, `memory.cached-incoherent`.
- **Partition**: the node compositions admitted together by the partition build
  (`kickos_compose(<system> PARTITION <node0.yaml>...)`, `kickos_compose partition`,
  `docs/design-m10-fleet.md` section 9): `partition.device`, `partition.port`,
  `partition.unserved`, `partition.region`, `partition.cached-incoherent`,
  `partition.gate-budget` and `partition.lone`. Before it, a device two nodes of a partition grant, and a `cached` region shared across
  nodes the chip file does not declare coherent, unless accepted. One composition's admission
  cannot see either: they need the partition's node compositions admitted together, the partition
  build `roadmap.md` places in M10.5.

The descriptions' own rules are `chip.*` and `board.*`, a name that resolves to nothing or to two
things is `name.*`, a packaged driver's roles and authority are `driver.*`, a port range among
them included, since a driver's descriptor takes every window role as a device window
(`driver.port-window`), and a manifest's own are `manifest.*`.

## The emitted table

One constant table per image, in memory every task can read. It carries no authority: every
capability it mentions is already in the kernel, and reading another task's entry reveals
addresses, not access.

Every index is 16 bits and every count names the Kconfig knob that bounds it. Sixteen bits is a
ceiling of the format, 65,535 entries of a kind, chosen to cover every configuration the kernel
supports; a larger one would need a new table version, which the header's version field is there
to carry. A string is an offset into the pool. None is 0xFFFF in every field that can hold it, so
such a field carries at most 0xFFFE: a kind's last index is 0xFFFE, and a line numbered 0xFFFF is
refused. A task's `driver` is its driver's index in the manifest's catalogue, which lists drivers
by name, and a `core_mask` of 0 leaves the task unpinned.

```text
header    magic u32, version u16, flags u16 (ends: never / on a task),
          ends_task u16, task_count u16, grant_count u16, ref_count u16, priv_count u16,
          region_count u16, strings_size u32, init_priority u8 (the init's `priority`),
          rsv0 u8, rsv1 u16, rsv2 u32, all zero; the arrays follow in this order: task[],
          grant[], ref[], priv[], region[], then the strings

task      name u32, block u32 (a packaged driver's ring block in bytes, or 0),
          entry (a pointer: user code, or a packaged driver's start),
          driver u16 (catalogue index, or none), ceiling u8 (the grant's priority ceiling: the
          task's `ceiling`, or a packaged driver's priority plus its highest thread offset),
          stack u32, priority u8, restart_max u8,
          flags u16 (console: a packaged driver that takes the console; block_uncached: its ring
          block is self-granted KOS_MEM_NOCACHE),
          core_mask u32, authority u32 (the word the spawn seats),
          first_grant u16, grant_count u16, cap_grant_count u16,
          first_use u16, use_count u16      -> ref[]: the tasks whose endpoints it uses
          first_watch u16, watch_count u16  -> ref[]: the tasks it watches

grant     kind u8 (endpoint_serve, endpoint_use, notification, window, ports, region, line,
          status), flags u8 (ro, uncached), cap_slot u16 (the slot a capability-bearing grant
          lands at, or none), name u32 (what the task looks it up by), path u32,
          target u16 (the served endpoint's task, or the region), window u16 (a window-kind
          grant's place in the spawn's window list, or none),
          base u64 (a window's PHYSICAL base, or a port range's first port),
          size u32 (bytes, or ports), line_index u16, line u16 (the source number the kernel
          takes), priv_first u16, priv_count u16 -> priv[]: the privileged registers inside it

ref       task u16
priv      offset u16, width u8 (1, 2 or 4 bytes): left out of the task's direct reach; the
          bits a write may carry are kernel policy and stay in the kernel's allowlist
region    name u32, size u32, flags u8 (uncached)
strings   the names, NUL-terminated
```

The composition's `heap` is not in the table: the link carries it.

`cap_grant_count` is what admission holds within `KICKOS_MAX_SPAWN_GRANTS`; `use` and `watch`
are what readiness and death reports follow. A generation per task, which the readiness rule
needs, is run-time state the init keeps, not part of the constant table.

The layout is `<kickos/sys/table.h>`, installed with the user API, since the `kos_self_t const*
self` a task receives points at its `task` record: the init and the lookup library read the table
through that header, and user code only passes `self` back. Reserved zero fields pad each record
to a multiple of four bytes, eight where it holds a 64-bit field, and the entry takes eight bytes
wherever a pointer takes four, so each record has one size and one set of field offsets on every
architecture, which the header asserts, and the arrays follow one another with no gap. The layout
version is `KICKOS_TABLE_VERSION`, which the build writes into the generated
`<kickos/sys/table_version.h>` and into the manifest's `abi`; the tool refuses a manifest whose
layout it does not emit. `kickos_compose emit` writes the table as one C source of designated
initializers, the same for the same inputs, and the system target's `kickos_table` points at its
header. It declares each entry `extern`, so a missing one is a link error naming it, each
declaration under a `#line` giving the entry's line in the composition, so a declaration the
compiler reports is reported against the composition, and an entry
that the source defines, or that its headers' `kos_`, `KOS_` and `KICKOS_` names could take, is
refused.

A task's grants are contiguous and in declaration order: the order its entry writes its fields
in, and each field's items in theirs. `serves` is an endpoint served, each of `uses` an endpoint
used whose target is the serving task, `watches` one notification named `/init/events` and the
read-only status window `/init/status`, the watcher's own status block, a crossing served or used a
`port` grant whose flags are the right the init delegates from the port the kernel seated in root,
`KOS_CAP_WAIT` to its server and `KOS_CAP_SIGNAL` to a user, its base the port and its target the
node the partition list names its server, ordering nothing, each of
`devices` a window or a port range, named by its path or, for a packaged driver, by its window
role, each of `maps` a region, `ro` as written, and each of `lines` a line by the name it is
bound to, a packaged driver's in the order of its line roles. They are of two sorts. A grant that
carries a capability, an endpoint served or used, a crossing, a notification or a line, is delegated at spawn,
and those grants alone are numbered: the i-th of them lands at slot `KOS_SPAWN_DELEGATED_CAP0 + i`, the slot the table records. A window, a port
range, a shared region or the status window is a mapping the spawn makes in table order, occupies
no capability slot, and records its place in the spawn's window list instead. A packaged driver's
grants record neither a slot nor a window's place, its `Descriptor` placing what each of its
threads receives. The status window's size is the count of tasks the watcher watches times the
manifest's status record size, rounded as a shared region is, and its base is 0: the init's
reservation places it. A partition region carries `KOS_TABLE_REGION_PARTITION` and its `offset` in
the user share, which the partition build places in node 0's declaration order at the alignment
every node mapping it needs; the init maps it at the share's base plus that offset and never
reserves it.

A privileged register's width is the kernel call that writes it, the chip file stating none: a
port register is one byte through `kos_port_reg_write`, a memory register a 32-bit word through
`kos_periph_reg_write`. A device's registers are one run of `priv[]`, which every window of that
device names.

**The table carries every device fact the init needs, so no kernel device catalogue is
required.** A window grant states its physical base and size, a port grant its first port and
count, and the kind says which: the init hands exactly those to the spawn, and the kernel checks
  them against its reserved blocks and one-holder rule. What the table never
carries is the address a translating board maps a window at, which the kernel chooses and
`kos_window_get` reads back: see the next section. A physical base is a chip fact, public in the
chip file, and granting nothing.

## The export manifest

What the installed kernel package carries so that a composition is checked without the kernel's
source tree: one YAML file, same subset, generated when the kernel build is configured, from its
resolved configuration, its protection unit, its descriptions and its driver declarations, and
installed beside the package's CMake files, where `kickos_compose` finds it.

| section | contents | from |
| --- | --- | --- |
| `abi` | `table`, the version of the emitted table's layout; `cap_reserved`, the capability indices the kernel reserves in every table; `symbol_prefix`, what the link spells before a C name, `""` or rx-elf's `_` | declared in `cmake/manifest.cmake`, `KICKOS_CAP_FIRST_DYNAMIC` in `cmake/cap_geometry.cmake`, and the prefix in the toolchain file of an ABI that adds one |
| `target` | board, chip, arch; cores, kernel cores, isolated cores; on an AMP node, its node, the partition's nodes, its ports and the bytes of its user share | the resolved Kconfig, and the node and port list as `cmake/amp_partition.cmake` parses them |
| `protection` | whether the build enforces its unit; the window rule, `pow2` (a power of two, naturally aligned), `granule` (a multiple of the smallest window, 16 bytes on a region build whose seam states no unit) or `none` (a translating build, whose page is its chip file's), and the smallest window, as the seams state them whether or not the build enforces, the kernel rounding every arena block and checking every device window by them either way; `KICKOS_MAX_THREAD_WINDOWS` as `thread_windows`; whether a fault ends its task rather than the system, as `fault_isolation` | `KICKOS_MEMORY_ENFORCED`, `KICKOS_FAULT_ISOLATION`, `KICKOS_HAVE_ASPACE`, the `arch_mpu_min_region` and `arch_mpu_region_pow2` seams as `cmake/boot_arena.cmake` reads them, and the resolved Kconfig |
| `pools` | every `KICKOS_MAX_*` but `KICKOS_MAX_THREAD_WINDOWS`, every `KICKOS_TASK_*_BUDGET`, `KICKOS_MAX_SPAWN_GRANTS`, `KICKOS_CAP_TABLE_SUPPLY`, `KICKOS_RAM_OWNER_SLOTS`, `KICKOS_ASPACE_RANGES` | the resolved Kconfig |
| `threads` | the priority range; `KICKOS_MIN_STACK_SIZE` as `min_stack`; `KICKOS_USER_STACK_SIZE`, the stack a runtime spawn defaults to, as `user_stack`; the idle and root stacks; `KICKOS_STACK_ALIGN` as `stack_align`; `stack_stride`, the block every stack is where the thread pointer is SP masked, or `none` | declared in `cmake/sched_geometry.cmake` and `cmake/stack_geometry.cmake`, the stride as the top-level `CMakeLists.txt` derives it, and the resolved Kconfig |
| `descriptions` | the board's chip and board files, by their path beside the manifest, on a board that has them; admission and emission against the manifest read the board and chip there and nowhere else | `platform/<chip>/<board>.yaml` and its chip file, copied beside the manifest |
| `default` | `composition`, the board's default composition, by its path beside the manifest, on a board that has one, which needs its descriptions | `boards/<board>/composition.yaml`, copied beside the manifest |
| `init` | `status_record_size`, one watched task's record in a watcher's status block, which sizes the watcher's `/init/status`; `private_record_size`, one record in its private block, which holds one per task and one per shared region; `free_regions`, the regions root's set holds past its static regions and its stack, which the init self-grants into on a region board | `cmake/init_geometry.cmake` beside `cmake/driver_geometry.cmake`; `KICKOS_MPU_MAX_REGIONS` in `cmake/mpu_geometry.cmake` less the static regions root's linker script bounds and its stack |
| `drivers` | the packaged driver catalogue: window and line roles, threads with their priority offset, stack, capabilities and badged copies, the thread that receives, the endpoints and notifications each creates, ring block and its memory type (`block_cache`, `cached` or `uncached`), endpoint posture, readiness barrier, whether it takes the console and whether that console is a USB device controller (`usb_device`), its start, and the libraries its clients link | `kickos_add_driver`, and what the shared bring-up creates as `cmake/driver_geometry.cmake` declares it |

The catalogue lists the drivers this build declares, and a composition naming a `driver` the
catalogue does not list is refused.

A sim build's smallest window is the page size of the host that generated its manifest, the sim's
`arch_mpu_min_region` being that host's `mprotect` granule, so a sim manifest describes the host
it was built on.

Two things are known only once the user's image is linked -- its thread-local block and its data
carve -- so admission runs twice: the composition against the manifest when the system target is
built, and the image's layout against the arena at the link, as linker-script `ASSERT`s the
system target brings. Both refuse with a message naming the rule.

## The host tool

One Python package under `tools/`, run under `uv` with its dependencies declared beside it,
and installed with the kernel package, with its declaration and lock, in a `compose` folder beside
the package's CMake files. It reads YAML through ruamel.yaml, which reads YAML 1.2,
so `yes` is a string the schema refuses as a boolean, refuses a duplicate key and keeps each
node's line; the subset's own refusals are checked on those nodes. A refusal reads
`<file>:<line>: <rule>: <message>`, and every rule has an arm that reddens when the rule is
removed. A run that refuses exits 3, so a caller tells a refusal from a tool that did not run. The kernel build runs it on the board's default composition against its own manifest,
so a build whose default no longer fits fails; `kickos_compose` runs it on the integrator's.
Without a manifest it checks a composition against the descriptions alone and says that only their
rules ran. Its `chip` subcommand writes the kernel's chip headers, tables and link values from a
chip file at configure, and with `--compare` asserts a hand-written header against them
([`design-m10-fleet.md`](design-m10-fleet.md), section 1.2).

## The Kconfig split

Classified against the rule that Kconfig configures the kernel and the composition configures
userspace:

- **Moves to the composition or is deleted, being userspace.** the service-list selection knob and
  the pin-map selection knob are deleted: services are tasks and pins are the board file's.
  `KICKOS_USER_HEAP_SIZE`, the libc heap the image carves, becomes the composition's required
  `heap`; the knob stays only for the images that link no system target, until M10.5 deletes it. The init-provider CMake cache entry is deleted: there is one init.
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
`kos_window_get` returns that address to the holder; on a region board it returns the
physical base. Tasks cannot assume that a window has its physical address or the same
address in another task. Randomized placement is a later policy, separate from kernel
placement.

Shared memory can be mapped at different addresses in different tasks. Data exchanged
through it therefore uses offsets, not stored pointers. Delegation does not promise to
preserve a reservation's address.

## The lookups

The init passes each task a `kos_self_t const* self` pointing to its table entry. Every
lookup takes `self`, so one entry function can serve several task instances and find
each instance's grants. Capabilities and windows are per thread, so the lookups answer for the
task's entry thread, the one the init spawned. A thread the task creates may read `self`, but
holds only what its own spawn delegates, which the entry passes it.

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
handles it, and notification bits can merge ready and death. The init therefore reads each
reported task's state rather than its bit, and checks that a pending client's server is still the
live instance that became ready, the generation being the task handle's, new at each start. A
restarted server becomes ready again at its own first receive; callers get `-KOS_EAGAIN` in the
gap.

## The init's walk

[`design-m10-target.md`](design-m10-target.md) states each step against the kernel calls it
makes.

1. At boot, reserve the init's private block, each watcher's status block, the shared regions,
   the ring blocks and, where SP is not masked, the user tasks' stacks, which the kernel hands out
   zeroed, so each region is zeroed once; create every served endpoint and keep it with HANDOUT and without WAIT;
   create each watcher's notification.
2. Scan tasks in file order. Leave a task pending while an endpoint it `uses` is unready.
   For a ready user task, create its kernel task, arm its watch, claim its lines, and spawn it
   with its windows in table order and its capability-bearing grants in declaration order.
3. For a packaged driver, follow its exported `Descriptor` with the endpoint created at boot.
   If it takes the console, publish it before claiming the UART line, then claim and attach its
   lines, spawn its threads and wait at its readiness barrier. Complete console handover by
   narrowing the init's capability to drop its receiving right and probing once. A restart of
   it repeats the handover, its publish through the init's HANDOUT seating the receiving right
   the narrow drops again. Admission requires the task `stdout` names before tasks that write
   to it.
4. If resident, wait for notifications. Read each reported task's state, a failed start being a
   death; start newly unblocked tasks in file order only when the server instance is still live
   and ready. Release the dead task, tell watchers, and restart it with the same grants while its
   count remains. Once retries are spent, drop HANDOUT so callers get `-KOS_ECONNREFUSED`, and
   mark its pending dependants as dependency-down.
5. End the system when the declared ending condition occurs.

**What one task costs**, which admission counts against the manifest's pools and budgets and the
init spends exactly:

- a kernel task; the boot's are one idle task per kernel core and the init's;
- a memory domain on a region board where it brings a ring block, the kernel keeping two, a
  mapped region being a window; on a translating board every task, the init's included, spends
  one;
- its threads: a user task's one, at its `stack`, or each thread a packaged driver's catalogue
  declares, on the kernel's default stack of `KICKOS_USER_STACK_SIZE`, each one whole stride where
  the thread pointer is SP masked; the init's own thread is root's slot, outside
  `KICKOS_MAX_THREADS`;
- the endpoint it serves, or a driver's catalogue endpoints, held for its users and by the init;
- a notification when it watches, or a driver's catalogue notifications;
- an IRQ binding per line, which the init claims and passes on at the spawn; a task taking a
  line, a packaged driver included, is pinned to its declared `core`, where its lines are
  claimed and waited on;
- at the spawn, a user task's capability-bearing grants, and each driver thread's catalogue
  capabilities, within `KICKOS_MAX_SPAWN_GRANTS`;
- its windows within `KICKOS_MAX_THREAD_WINDOWS`: its devices, the regions it maps and, for a
  watcher, `/init/status`;
- on a translating board, range records of its own space within `KICKOS_ASPACE_RANGES`: two for
  its image, one per thread's stack, one per window, and one for its data mapping, a user task's
  stack block, on which its one thread runs, or a driver's ring block.

The init keeps for life its own notification, each AMP port, every served endpoint, a console
driver's included, with HANDOUT, and each watcher's notification. While it starts or restarts a
task it holds besides that task's lines and one badged copy of its own notification, and for a
driver with a notification that notification and the most badged copies one step of its bring-up
holds: one while it binds a line, or the most one thread's spawn mints, which the catalogue
declares per thread. Its budgets and its capability table, within `KICKOS_CAP_TABLE_SUPPLY`, are
counted at what it keeps plus the most one start holds, whatever order the starts come in.

The init reserves, once for life, its private block, the task count plus the shared region
count times the private record size the manifest's `init` section declares, each watcher's
status block, the count of tasks it watches times the status record size it declares, each
shared region, each ring block and, where the thread pointer is not SP masked, each user task's
stack. On a region board it self-grants its private block, each status block and each ring block
into root's region set, whose `KICKOS_MPU_MAX_REGIONS`
root's code, its static data where the build carves that window, and its stack already hold, a
self-grant spending a region whether or not the build enforces, root being unprivileged on every
build; and where the build enforces its unit each reservation spends one of
`KICKOS_RAM_OWNER_SLOTS`. On a translating
board its own space holds its image's two range records, its root stack's and one per
reservation, a self-grant taking the range its reservation holds, within `KICKOS_ASPACE_RANGES`.

A user task's `stack` is at least `KICKOS_MIN_STACK_SIZE` and a multiple of `KICKOS_STACK_ALIGN`;
the thread-local block on top of that floor is a fact of the linked image, so the link-time
`ASSERT`s M10.4 brings check it. Where the thread pointer is SP masked every stack is one
stride-sized, stride-aligned block, so there a `stack` is a requested minimum: one above the
stride is refused, and the init gives each thread a whole stride and never hands the
composition's figure over as a caller stack. On a region board the idle and root stacks, then the
init's reservations in the order above, then the kernel's default stacks of
`KICKOS_USER_STACK_SIZE` in start order, each on the stride's alignment where SP is masked, are
carved from the chip file's arena entry, each rounded and aligned as the allocator places it; the
heap is the link's, below the arena, and the link's `ASSERT` stays the final word on the arena.

## Beyond boot: the nested init

A task with the required authority can create tasks and map memory at run time; the
boot init is one user of those kernel calls. It may delegate task-creation authority to
a nested init. A nested init receives readiness and death reports for the tasks it
creates and can use the same restart rules. No kernel or provisioning path singles out
the boot init.

The table's 16-bit indices describe the configured system, while kernel pool limits
remain configurable. A per-spawn grant bound limits one operation, not the lifetime
number of tasks.
