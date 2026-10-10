<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M10.5: the fleet

> **Status: ACTIVE.** This specifies how the whole fleet moves onto compositions: chip files with
> the kernel's chip headers generated from them, every board's files and default composition,
> every app and test image on a composition, packaged drivers taking their lines from the
> composition, the partition build, x86_64 through `add_executable`, and the deletion of every
> older way to compose a system. It builds on
> [`design-m10-composition.md`](design-m10-composition.md) and
> [`design-m10-target.md`](design-m10-target.md). `roadmap.md` assigns the steps, M10.5.2 to
> M10.5.14.

## The whole design in one paragraph

Every chip has a chip file and every board a board file and a default composition, the sim
included. The host tool that admits compositions also reads the chip file at configure and writes
the kernel's chip headers, tables and link values from it, so a hardware fact is written once.
Every image links `KickOS::kernel` and exactly one system target: the board's default when `main`
needs nothing more, otherwise a composition the app owns. A packaged driver names roles and the
composition binds them to lines. The node compositions of an AMP partition are admitted together
on every node, which derives who owns each device behind a partition gate. Then the service lists,
pin maps, the default init, the old leaves and the heap knob are deleted, since nothing composes
a system through them any more.

## 1. Chip files and the headers generated from them

### 1.1 What the chip file gains

The chip file grows in place, `version: 1`, by the fields a generated header needs and no more:

| field | where | what it states | emitted as |
| --- | --- | --- | --- |
| `manual` | top level | the document the values come from, with its version | the header's opening comment |
| `ref` | any device, line, block or memory entry | where in that manual the value is | a comment beside the symbol |
| `blocks` | a device | named sub-blocks of its window, each an offset (the XMC's SCU trap, reset, clock, oscillator and PLL units) | one base per block |
| `owner: kernel` | a device | already in the schema; every device the kernel holds for life is now described, its line too (the F411's TIM2 at 28, the XMC's flash controller) | a reserved row and its symbols |
| `link` | a memory entry | the linker region it is and its access, `{ region: FLASH, access: rx }` | the link values of 1.2 |
| `interrupts` | top level | `count`, the controller's line count; `soft_only_from`, the first line no hardware raises; `free_from`, the first line no device uses; `vectors`, the RX's INTB table size | `chip_limits.h` |
| `cycle_counter` | top level | `hz: 0` where the counter has no fixed rate (QEMU), `glitches: true` where a read can glitch (the XMC) | `chip_limits.h` |
| `c` | top level | the C++ namespace where it is not `kickos::<chip>` (the XMC's `kickos::xmc`, the RX's `kickos::rx`) and the line enum's declaration where it is not `irq_num` | the header's form |
| `symbol` | any entry | the C name where today's differs from the derived one | that name |

A symbol's name is derived: a device's base is `<DEVICE>_BASE`, a channel's
`<DEVICE>_<CHANNEL>_BASE`, a block's `<DEVICE>_<BLOCK>_BASE`, a repeated device's first instance
`<DEVICE>0_BASE` with `<DEVICE>_STRIDE`, a memory entry's `<ENTRY>_BASE`, and a line
`<DEVICE>_<LINE>`. `symbol` exists only so the generated headers keep every name an includer
uses today, which is what lets each chip switch with no includer edited but those 1.3 names.

A line in the chip file is still the number the kernel takes. On a multi-cluster part the
generator adds the built cluster's `line_offset`, as admission does.

Four more schema facts the fleet needs, none of them a field of the C:

- **A part with two memory maps is two chips.** QEMU's AN505 behind `qemu-m33` has its own map
  and a PMSAv8 unit, where the other MPS2 machines share one map and PMSAv7. It becomes a chip
  backend of its own over an `mps2` family directory, as the RP and STM32 pairs already are, so
  every chip file names exactly one backend and the CMake test of the board's name for PMSAv8
  goes.
- **The sim is a chip.** Its unit is `mprotect`, a region unit whose granule is the generating
  host's page, which the manifest already exports. Its arena entry states a size and no base,
  the host placing it, and that is the only place `base` may be absent. Its console and its
  loopback UART are devices marked `host: true`, reached through the host rather than a window,
  the UART carrying its line.
- **A partition gate is a list of gates**, each with the device that programs it, how many
  assignments it holds (`regions`, where the gate is a region table) and the address `ranges` it
  fronts, so every device's gate follows from its address, a device straddling two being refused
  as `chip.gate-straddle` already refuses one for a `device_gate`. A gate of one register per
  device, as the RP2350's ACCESSCTRL is, states each device's register offset instead (section 9).
- **The `/amp` namespace and partition regions** are composition fields (section 9).

M10.5 writes each of these into the schema tables of
[`design-m10-composition.md`](design-m10-composition.md) as it lands them, with the new grant kind.

### 1.2 The generator

The generator is a subcommand of the host tool, `kickos_compose chip`, under the same `uv`
project and lock. It reads the board file and the chip file it names, refuses a chip file the
descriptions' rules refuse, and writes, for the built chip and cluster:

| output | form | from | replaces |
| --- | --- | --- | --- |
| `chip_mmap.h` | `constexpr uintptr_t` in the chip's namespace, guard unchanged | devices, channels, blocks, repeated devices, memory entries | the hand-written header, and the literals inline in `chip_*.cc` on the chips that have none |
| `irq.h` | the line enum, form and namespace unchanged | every line, kernel-owned ones included | the hand-written `irq.h`; the ESP32-C6's CPU interrupt enum, which nothing referenced, is dropped rather than moved |
| `chip_limits.h` | pure `#define`, as `startup.S` includes it | `interrupts`, `cycle_counter` | the hand-written header |
| `chip_layout.h` | pure `#define`, for linker scripts and assembly | each memory entry's base and size, each device base, each line | the `MEMORY{}` literals, `boot_layout.ld.h`'s device addresses and the RP2350's doorbell line |
| `chip_tables.h` | `constexpr` arrays of `arch_reserved_block` | reserved rows: every `owner: kernel` window and port range; window apertures: every other device window, on a translating chip; port apertures: every other port range | each chip's static arrays |
| `chip.cmake` | CMake facts | `protection.unit`, each link region's origin and length | `mpu.cmake`, `aspace.cmake` and the microbit's scrape of its script's RAM length |
| `board_pins.h` | pure `#define` | the board file's console and kernel LED (section 2.2) | the console and LED pins in each chip's C and the F411 pair's LED half of `board_wiring.h` |
| `board_buses.h` | `constexpr` per bus: its device base, each pin's port, bit and the mux value of the selector carrying that device, each chip select's port and bit | the board file's `buses` | the bus pin numbers a client or driver wrote by hand |

```text
<build>/generated/include/kickos/chip_mmap.h    on every include path, installed
<build>/generated/include/kickos/chip_limits.h  on every include path, installed
<build>/generated/chip/irq.h                    the chip archive's and REGDIR's include path
<build>/generated/chip/chip_layout.h            the linker script's preprocessor path too
<build>/generated/chip/chip_tables.h
<build>/generated/chip/chip.cmake               included by the root CMakeLists.txt
<build>/generated/chip/board_pins.h             the chip archive's include path
<build>/generated/chip/board_buses.h
```

**The link values belong in the chip file** because the arena admission carves from and the RAM
region the link places are one fact. Each chip script writes its `MEMORY{}` from
`chip_layout.h`; the AMP arithmetic that slices a node's share stays in the script, being the
partition's, which Kconfig states.

**The tables are read in place.** `arch_reserved_blocks`, `arch_window_apertures` and
`arch_port_apertures` stop copying into a caller's array: each answers a span over its generated
array, one definition in `arch/common/` serving every chip, and the kernel's stack copies in the
grant and system-call paths go, with the fixed bound of 8 rows they were sized by, which the
ESP32-C6's kernel-owned devices are past and no longer have to pass. The
span is `struct arch_reserved_span`, a pointer and a count, rather than `std::span`: `<span>`
pulls `<limits>`, whose float members a translation unit built with `-mgeneral-regs-only` for a
hard-float ABI refuses, and such units include `arch.h`.

```c
struct arch_reserved_span arch_reserved_blocks(void);
struct arch_reserved_span arch_window_apertures(void);
struct arch_reserved_span arch_bus_master_apertures(void);
struct arch_reserved_span arch_port_apertures(void);
```

`arch_bus_master_apertures` is the rows of the windows a grantable `bus_master` device covers,
on a translating chip and a region chip alike. The kernel opens a spawn's device window over one
only for a spawner holding `KOS_AUTH_BUS_MASTER`, so a hand-written spawn holding
`KOS_AUTH_MEMORY` and `KOS_AUTH_SYSTEM` without it is refused what admission refuses a composition
that does not accept `bus_master`. The init holds the authority, and a spawner passes it on only if it holds
it, as every other authority. A composition names it `bus_master` in a task's `authority`;
`accepts: [bus_master]` stays the composition's consent.

**The rows follow the file, so the kernel and admission agree.** Today the kernel reserves fewer
devices than the files mark `owner: kernel` (the C6, the F411's GPIO, the K64F's ports, the
i.MX 8M Plus's RDC, CSU and MU), and grants windows narrower than the files call grantable (virt
arm64 admits only the PL011 and PL031, q35 keeps COM1 out of its port apertures). Generated, a
grant the kernel refuses is one admission refuses, and the reverse. Where the C is right and the
file wrong, as the XMC's CCU40 and SCU are 0x1000 in the C and 0x4000 in the file, the manual
decides and the file carries its `ref`.

**Every translating chip's devices are audited before they become apertures**, since an aperture
is a window the kernel admits for any spawner holding memory authority, a nested init included. A
device that reaches what no grant may, as `fw_cfg` on virt arm64 does by writing guest RAM through
its DMA interface, is either marked `owner: kernel` or left out of the file, and the file says
which and why. The audit's verdicts:

| chip | non-kernel devices today | verdict |
| --- | --- | --- |
| `virt_arm64` | `uart0`, `rtc`, `gpio`, `virtio` | apertures; `virtio` is a `bus_master`, which admission refuses unless accepted; `fw_cfg` stays out of the file |
| `virt_rv64` | `uart0`, `rtc` | apertures, as today |
| `q35` | `cmos_rtc`, `com1`, `com2`, `hpet` | apertures; `com1` joins them under the console exception below |
| `imx8mp` | `uart1`, `sai5`, `sai6`, `sdma1`, `enet1` | apertures; the CCM and the SRC, which reach every peripheral on the die, join the file as `owner: kernel` |

**The console device is an aperture like any other.** Admission refuses it to every task but the
one `stdout` names (`ownership.console`); in the kernel its guard is the window's admissibility,
the reserved rows and the one-holder rule, as on every region board today. On q35 the kernel
still writes COM1 itself at the end: `arch_shutdown` prints the exit line there whatever holds it.

**`chip.cmake` carries the backend, and Kconfig the decision to enforce.** A region unit names
its backend (`sysmpu` the K64F's, `pmsav8` the RP2350's and the AN505's) and `mmu` sets the
translating flag. Kconfig's `CHIP_*` and its `select HAS_MPU` or `HAS_ASPACE` stay the selection,
the enforcing flag following `HAS_MPU` rather than being stated a second time per chip. The
configure-time pair checks become one static gate over every chip: a chip selecting `HAS_MPU` has
a region unit in its file, a chip has `mmu` exactly when it selects `HAS_ASPACE`, and a chip whose
file states `none` selects neither. A part whose unit the kernel does not drive still states it,
with `driven: false`, exactly when its chip selects no `HAS_MPU`, so enabling the unit is a change
to both; its builds need `no_protection`, which admission already derives from a build that
enforces nothing.

**The build regenerates on an edit.** The generator runs at configure, before `arch/`, with the
chip file, the board file and the tool's sources in `CMAKE_CONFIGURE_DEPENDS`, and writes each
output only when its bytes change, as `cmake/manifest.cmake` and `cmake/compose.cmake` already do.
A YAML edit reconfigures, rewrites what changed and recompiles what includes it; nothing else
rebuilds. Every board's configure now needs `uv`, which every build with a default composition
already does.

### 1.3 What stays hand-written

What the chip code decides rather than what the part is:

- register layouts, offsets and bit fields, under each chip's `regs/`;
- a constant derived from two facts or not a fact of the map at all: the XMC's USIC channel
  stride and module span, the RP's atomic alias offsets and its `rp2xxx` namespace aliases, a core
  register. Each moves into the one chip source that uses it, written over the generated names;
- interrupt routing: the ESP32's matrix sources and CPU interrupts, and the RX72M's group lines,
  whose enum `irq.h` computes today. Each moves into a routing header beside the chip sources, and
  its includers include that header: the one place a switch edits includers. The ESP32-C6's CPU
  interrupt enum had no includer, so it was dropped;
- clock trees, init sequences, pin mux code and the privileged-register write allowlist; which
  pins the mux code writes is the board file's (section 2.2);
- the board wiring headers (`board_wiring.h` on the F411 pair), which hold the crystal, which
  the board file has no field for;
- the translating chips' virtual layout (`boot_layout.ld.h` keeps its kernel addresses) and the AMP
  slicing in the chip scripts;
- `caps.cmake` and `terminate.cmake`, which state what the kernel's tracer and exit port do, not
  what the part is.

A hand-written symbol that nothing references is dropped rather than described.

### 1.4 The proof, per chip

1. **The symbol compare.** `kickos_compose chip --compare <header>` writes a C++ source that
   includes the hand-written header and `static_assert`s every generated constant's value and
   type, every enumerator's value and the enum's underlying type, and every `#define` by `#if`.
   It compiles with the chip's compiler. A hand-written symbol the generator lacks is found by
   the next step, since its includer stops compiling.
2. **The emitted-code compare.** Every preset of the chip builds twice from one tree, with the
   hand-written headers and with the generated ones on the include path, and every linked
   image's loaded sections compare byte for byte with the bytes of `kickos_build_time` and
   `kickos_build_commit` masked. Both builds are made from one `git archive` of the tree, so the
   commit label is `nogit` in both and every stamp has one length, and with `-ffile-prefix-map`
   mapping each build directory to one name, so no path differs. **The null control** is the
   hand-written build made twice in two directories, which must compare equal, so a difference is
   never the instrument's. **The plant** changes one generated value, which must show, so an equal
   compare is never vacuous.
3. Then the hand-written headers are deleted in the same commit.

The tables are not byte-identical where the rows change (1.2), so they switch in a second
commit per chip, after the headers, which also brings the span API. That commit is proven on
target: the selftest's reserved arm refuses a device grant on the chip's first reserved block,
and the aperture rows are proven by an admitted grant spawning
on QEMU's translating boards.

### 1.5 The order

1. The nine described chips: `xmc4800`, `stm32f411`, `mk64f`, `esp32c6`, `virt_rv32`, then the
   translating `virt_arm64`, `virt_rv64`, `q35` and `imx8mp` (M10.5.2).
2. The rest, by family (M10.5.4): the STM32 pair `stm32f103` and `stm32f302`; the RP pair
   `rp2040` and `rp2350`; `imxrt1062`; `sam3x8e`; `nrf51`; `mps2` and the AN505; `rx72m`;
   `esp32`; the sim.

Each family lands as one commit per chip, headers then tables, so a red compare names one chip.

## 2. Board files and default compositions

Every board gets `platform/<chip>/<board>.yaml` and `boards/<board>/composition.yaml`, the sim and
the i.MX 8M Plus EVK included. A board file states what the board's wiring headers, its
`boards.md` page and its pin uses say today: the console device and pins, the LEDs and which one
the kernel owns, the buses and parts soldered on, and the pins it spends. Nothing is guessed: a
fact no source in the tree states is left out, and a device no file names is absent.

**A default composition is the same file everywhere:** the kernel console, one task `main` with
`entry: kickos_main`, `authority: [memory, system, tasks]`, `ends: main`, and the board's `heap`,
which is the figure its `base` preset carries today. `main` runs at priority 2 under `ceiling: 31`,
the build's `KICKOS_PRIO_MAX`: a plain `main` runs where root ran and may spawn threads above
itself, as root's could. A preset that cannot afford that heap, as an
`-st` preset carving none cannot, links what it runs through a composition of its own, which
states its own heap: the selftest's does (section 4.4). `accepts` lists what some build of the
board needs, as the existing ones do: `no_protection` where a flat preset exists or the unit is
`none`, `no_privilege_split` where a core has none. It fits every preset that links it, the AMP
ones included: one file, admitted against each preset's own manifest.

**An AMP node composes like any image.** A composition names no node: the node is the kernel
build's, which its manifest states. Every node of a partition built with no app of its own links
the board's default, so a default partition is the default composition on each node. **A
cluster is named**, `cluster: a53` on the i.MX 8M Plus EVK, the only cluster built; the C6's two
cores share an `arch`, so its composition names none and accepts the union of their limitations.

**The admission gate covers every board three ways:**

1. `platform_compositions` admits every `boards/*/composition.yaml` against the descriptions, in
   the code-quality job, as today.
2. Every configure admits its board's default against its own manifest, which every CI job and
   the fleet sweep do for every preset they build.
3. A new mode of `tests/static/check_platform.sh` holds coverage: every chip backend has a chip
   file, every board directory a board file and a default composition. A test may enumerate the
   tree; a CMake function still may not.

Once every board has a default, a board without one is refused at configure, as a board without
a `base` defconfig is, and the `KickOS::system_default` stub and its `system_link_stub` gate go.

### 2.1 Each board's own Kconfig

Classified by the rule that Kconfig configures the kernel and the composition configures
userspace, as the composition design leaves for when each board moves:

| board | its own option | side | verdict |
| --- | --- | --- | --- |
| `xmc4800-relax`, `frdmk64f` | the service-list selection knob, defaulted on the enforcing posture | userspace | deleted with the service lists (section 6): a service is a task of a composition |
| `xmc4800-relax`, `frdmk64f`, `picopi`, `f302nucleo` | the pin-map selection knob | userspace | deleted with the pin maps (section 6): a task muxes its own pins and the board file states the wiring |
| `imx8mp-evk`, `qemu-arm64`, `qemu-riscv64` | `KICKOS_USER_HEAP_SIZE`, 65536 | userspace | stays until the heap knob is deleted (section 6); the figure is already their default composition's `heap` |
| `microbit` | its full-newlib choice, which C library the package links | kernel | generalised in M10.5.5: the board descriptor names the nano profile (`KICKOS_BOARD_NEWLIB`, also on `f302nucleo` and `bluepill-c8`) and the fleet's `KICKOS_FULL_NEWLIB` chooses the full one; it is what the kernel package is built with, a fact of the export every image links |
| `picopi`, `pizero2350`, `teensy41` | none; chip init brings the USB console's clock tree up in an image whose stdout is a USB device console driver | kernel | nothing to configure: the driver's `kickos_usb_device_console` (`<kickos/usb_console.h>`) is what the chip reads |
| `blackpill`, `bluepill-c8`, `due`, `esp32-wroom`, `esp32c6-wroom`, `f411disco`, `qemu`, `qemu-m3`, `qemu-m7`, `qemu-m33`, `qemu-riscv`, `qemu-x86_64`, `rx72m`, `sim` | none | | nothing to classify |

### 2.2 The board's pins in the kernel

The board file is the one statement of the kernel console's pins and of the LED the kernel
drives, and of the pins it reserves. The generator writes `board_pins.h` from it, a pure `#define` header the chip archive
includes:

| macro | from |
| --- | --- |
| `KICKOS_BOARD_CONSOLE_BASE`, `_SIZE` | the console device's window, a channel's or an instance's where the path names one |
| `KICKOS_BOARD_CONSOLE_<ROLE>_PORT`, `_PORT_BASE`, `_BIT` | the console pin's `gpio` function: its port's number where the chip has more than one port (the instance of a port device that repeats, else the number the port device's name ends in, the i.MX RT's `gpio1`), that port's base and the bit; the RX72M's `port` device is one port, so it has no `_PORT` and its `_BIT` is the port index times 8 plus the pin |
| `KICKOS_BOARD_CONSOLE_<ROLE>_SELECT` | the value of the selector whose function is `<device>.<role>`: the number the selector ends in (`f2`, `alt3`, `af7`, `psel11`), or a one-letter selector's place from `a` = 0 |
| `KICKOS_BOARD_CONSOLE_<ROLE>_INPUT_SELECT` | the function's `input_select`, where the chip file states one |
| `KICKOS_BOARD_LED_PORT`, `_PORT_BASE`, `_BIT` | the `owner: kernel` LED's pin |
| `KICKOS_BOARD_LED_ACTIVE_LOW`, or `KICKOS_BOARD_LED_ADDRESSABLE` | its `active` level, or `kind: addressable` for an LED sent its state as data, which has none |
| `KICKOS_BOARD_KERNEL_PINS(PIN)` | every pin the kernel holds, the console pins, the `owner: kernel` LED and the reserved pins, as `PIN(port, bit)`, the port numbered as `_PORT` is and 0 on a chip of one port |
| `KICKOS_CHIP_PINS(PIN)` | every pin of the chip file with a `gpio` function, as `PIN(port, bit)`; the i.MX RT's `arch_pinmux_set` muxes these pads and no other |

A role is the signal its pin carries, so a chip's code reads the roles its console device has:
`TX` and `RX`, the RX72M's `TXD` and `RXD`, the XMC's `DOUT0` and `DX0`, whose DX0 input line
comes from the pin function's `input_select`. A board without a pin
or a kernel LED gets none of its macros, and a chip that reads one fails to compile there. Every
pin the header names has a `gpio` function, which admission requires of a console, LED or
reserved pin (`board.pin-not-gpio`).

The chip code keeps how a pin is muxed: the register, its field, the pad state, the clock gate.
It reads which pin and which function from the header, `static_assert`s that the console is the
device it drives and that the selector is one its mux code serves (the STM32F103's unremapped
USART1, the SAM3X's peripheral A, the ESP chips' reset IO MUX function), and refuses every pin
`KICKOS_BOARD_KERNEL_PINS` names in `arch_pinmux_set` through one guard,
`kickos::board_pin_kernel_owned` (`arch/common/pin_guard.h`). The list is a macro rather than an
array so that the guard expands it into one condition the compiler folds. The RX72M names a pin to
the guard by port 0 and its bit along the port rows. A value
per pin that the selector does not carry is the chip file's,
on the pin function: the i.MX RT's DAISY input, `alt2: { function: lpuart6.rx, input_select: 1,
ref: ... }`, and the XMC's DX0 input line, `in: { function: usic0.ch0.dx0, input_select: 1 }`.
Admission already refuses a console pin whose chip file entry carries another signal
(`board.pin-signal`), a board file naming two kernel LEDs (`board.led-owner`), and a kernel LED
whose `kind` is not the one the chip's code drives, the chip file's `c: { led: addressable }`
where it is not `level` (`board.led-kind`).

**The proof is three parts.** Each chip's switch was proven by section 1.4's emitted-code
compare over every preset of the chip, with a plant that moved the board's console TX pin and,
where it has one, its LED, so a pin the code reads from the header moved the image and a pin
still named in the C would not have. That compare was run once, at the switch. The generator's
tests (`tools/compose/tests/test_chip.py`) pin the header each board file writes, and the
admission arms (`tools/compose/tests/test_arms.py`) the refusals above. The chip code's
`static_assert`s hold, on every build, the console device and the selector against the header.

## 3. The smallest boards, measured first

Before any app moves on `microbit`, `f302nucleo` and `bluepill-c8`, M10.5.5 measures what the
move costs there. Three builds of one tree, one plain C `main` that prints a line and returns:

| image | links | isolates |
| --- | --- | --- |
| A | the `kickos` leaf, `main` as root under the default init | today's baseline |
| B | the `kickos_cxx` leaf, `main` as root | dropping the freestanding leaf: full C++ flags, `libstdc++` and `libsupc++` in the group, the runtime objects |
| C | `KickOS::kernel` and `KickOS::system_default` | the init, the table and `main` as a task |

The same three for the largest selftest image of each `-st` preset, C there being a size link
only: the composition section 4.4 gives the selftest, written first so the link has a table, its
arms not yet moved off root.

**What is measured**, per image: flash (loaded text, read-only data and data's load image), static
RAM (data, bss, the heap carve), the arena the boot and the init spend (`kickos_compose cost` and
the chip script's arena asserts), and the thread slots left under `KICKOS_MAX_THREADS`. **The
instrument** is `size -A` over each linked ELF, B and C as deltas against A from the same tree
with the stamp masked as 1.4 masks it, and A built twice as the null control. The table went to
archived `M10.5_smallest_boards_meas.md` with the presets, toolchain and tree it was taken on.

**What the selftest as a task costs is decided here too, per board.** Run by the init rather than
as root, it spends a task slot and its entry thread from `KICKOS_MAX_THREADS`, where root's slot
was outside both, and its capability table is a child's, `KICKOS_CAP_CHILD_WIDTH` wide, where
root's was summed. Its declared peak is 5 slots held at once plus 3 the deadlock arm skips without,
above the `KICKOS_CAP_FIRST_DYNAMIC` reserved ones, and its composition delegates no capability at
the spawn. A self-test defconfig states `KICKOS_MAX_SPAWN_GRANTS=9` on a board whose
`KICKOS_CAP_TABLE_SUPPLY` is 10 or more, a child's 10 slots holding the 2 reserved, the 5 and the 3.
`bluepill-c8` and `f302nucleo` state a supply of 7 and stay at 6, a child's 7 holding the 2 and
the 5 exactly, and their gates declare `mutex_deadlock` a skip. On `microbit` the 9 costs each
selftest image 560 B of `.bss` and 312 to 856 B of `.text` against 6, measured on two builds of
one tree, and no image loses a 2 KiB arena block: `selftest` keeps 320 B before the next block
boundary and the other four 928 to 1,440 B. The row decides each small board: `KICKOS_MAX_THREADS` and `KICKOS_MAX_TASKS` raised by
one where its RAM allows, recording nothing again; a board whose child width cannot hold the peak
raises `KICKOS_MAX_SPAWN_GRANTS` where its RAM allows; a misfit goes to the maintainer.

**A misfit is a finding, not a case.** If a board cannot carry a default system or the selftest
under its composition, the figures go to the maintainer as a decision: no stripped init, no
freestanding leaf kept for small parts, no composition rule relaxed for them. The one answer
already ruled is the selftest's: an image that overflows splits into one more region rather than
dropping arms (maintainer, 2026-10-03).

## 4. Every app and test image on a composition

### 4.1 Three classes

1. **Default**: links `KickOS::kernel` and `KickOS::system_default`. Its `main` needs no authority
   beyond `memory`, `system` and `tasks`, writes to the kernel console, and ends the system when it
   returns, a `main` that parks forever never ending it.
2. **Own composition**: links `KickOS::kernel` and the system its `kickos_compose` builds from the
   composition its CMake passes to `kickos_app_system` (`user/apps/CMakeLists.txt`), the root
   `CMakeLists.txt` composing each once the manifest is written. It is `<app>/system.yaml`, naming
   no board and taking its heap, stack and platform-wide `accepts` from the build board's default
   but stating each `entry` task's `ceiling`, and `<app>/systems/<board>.yaml` only where the
   devices it names differ by board. The app names
   the authority, devices, lines and drivers it uses; nothing is mapped or claimed by number at
   run time that the composition can state.
3. **Root or old-init test**: retargeted onto its composed equivalent, or deleted where its witness
   survives elsewhere.

Under M10.4's task end, `main` returning stops every other member of its task, so the four apps
whose child never exits (`initdemo`, `tele_pingpong`, `drvdeath`, `rootfault`) end through the
`ends: main` their composition states, the default's or their own.

### 4.2 `user/apps/common/`

| app | class | what its composition holds, or where its witness goes |
| --- | --- | --- |
| `ampping` | 2 | one composition per node; the crossings it calls or serves under `/amp` (section 9); `ends: main` on node 0 and `ends: never` on the serving nodes |
| `deadstack`, `kernelhalf`, `stackguard` | 1 | the fault is `main`'s, or above one core its reader thread's; the gate reads the fault report and `KOS_EXIT_FAULT` |
| `bench` (three images) | 2 | `irq` beside the default's three, for the lines it raises past `KICKOS_IRQ_FREE_BASE`, claimed at run time; `main`'s table being a child's, it closes each handle once its peers hold their own |
| `benchauth` | 2 | `memory`, `system`, `irq`; its root-against-child arms become task-against-child |
| `blink`, `chaincheck`, `chaincheck_float`, `chaincheck_full`, `clocksoak`, `cxxtest`, `fp_switch`, `hello`, `hello_c` | 1 | |
| `errnoprobe` | 1 | its arm on a core's first thread runs as a constructor on root, which runs the app's constructors and is still that thread; a switch-in before it is not excluded |
| `clockretune` | 2 | `system`, `pstate` |
| `drvdeath` | 3 | retargeted: `stdout` names the packaged `simcon` with no restart; each death knob is a death or a failed start the init reports, the kernel console back each time; `main` uses and watches `simcon`, so a failed start leaves it dependency-down, the system ending with `KOS_EXIT_CANCELLED`, and a death is read off `/init/events`; the two-thread knob's register holder is the driver task's entry, which the app releases through the driver's test hook; the root-identity arm keeps root's handle, the init's thread, `kos_thread_self` answering none on one core |
| `fault`, `panicgate`, `pspguard`, `ringpriv`, `specfault` | 1 | `main` is the unprivileged faulter or prober that root was; `specfault` is built only where memory is enforced |
| `stress` | 1 | `main`'s table being a child's, it closes its copies of a pair's semaphores once the pair holds them, a start gate kicking the pairs |
| `stackdepth` | 1 | it still reads the kernel stacks' high-water mark: its deep spawn chain runs from `main`, and the init's own depth on root's stack is the composition witness's measurement, not this app's |
| `faultsurvive` | 1, and 2 for `_published` | the published variant's `stdout` names `simcon` |
| `gpioblink` | 2 | the LED port's window, `pinmux` to mux its own pin, `coarse_gate` where the port holds pins the board spends; the blinking thread is the task's entry; built on the two boards whose GPIO layout it carries, `xmc4800-relax` and `frdmk64f` |
| `initdemo` | 3 | retargeted: `console` authority, so the task publishes its own endpoint as root did, and the misroute arms are unchanged |
| `libc_exit` | 1 | a worker's `exit` ends the worker, `main`'s ends the system |
| `mpu_fault`, `objbudget`, `rebootdemo`, `slaypeer`, `taskleave`, `tlsprobe`, `tele_flood`, `tele_pingpong` | 1 | |
| `pubpanic`, `simconabi` | 2 | `stdout` names `simcon` |
| `reclaimwit` | 2 | `memory`, `system`, `console`, `tasks`; `stdout: kernel` is its precondition |
| `rootauth` | 3 | retargeted: a task declaring `[memory, system, pinmux]` meets the same refusals, witnessing the composition's authority reaching the spawn |
| `rootfault` | 3 | retargeted on the default: `main` writes the domain of a task it created and faults |
| `rootgone` | 3 | deleted with its gate, `check_rootgone.sh`: the init never dies; root's retired slot reaching no later spawn is the host unit test `tests/unit/rootslot/root_retire.cc`, and a dead task's handle and authority reaching nobody is the selftest's `task_exit_*`, `task_slay_*` and `task_creator_gate` arms |
| `sched_exit` | 3 | retargeted on the default: the root-exit arm is `main`'s task end, and `main`'s own call of the root-only last-thread wait answers `-KOS_EPERM` until that wait is deleted (section 6) |
| `selftest` | 2 | section 4.4 |
| `sysdefault` | 1 | already |
| `trapnest` | 2 | `irq` |
| `uartloop` | 2 | the packaged `simuart` and a task using its endpoint |
| `usbcdcwit` | 2 | `stdout` names the packaged `rpusb` or `rt1062usb`, on a build whose service list publishes over USB |

### 4.3 The per-board apps

| app | class | what its composition holds, or where its witness goes |
| --- | --- | --- |
| `esp32c6-wroom/c6blink` | 2 | the GPIO bank and `pinmux`; the ungranted poke is a child thread's fault. No `coarse_gate`, although the bank holds pins the board spends: the platform-wide `no_protection` the LP core's cluster brings subsumes it, and admission refuses it as unneeded (`enforcement.unneeded`) |
| `esp32c6-wroom/c6intpri` | 3 | deleted: it probes INTPRI, which the kernel owns and admission refuses to grant; what it found is stated in the chip file and the chip code, and the inject doorbell it identified runs in every C6 AMP capture |
| `esp32c6-wroom/c6lpprobe` | 1 | its flat build accepts `no_protection` already |
| `esp32c6-wroom/c6txidle` | 1 | the default composition in the flat build, reading what a kernel-side probe recorded in M-mode: a U-mode thread's ungranted UART0 access passes no APM |
| `esp32-wroom/lx6smp` | 3 | deleted: it starts the APP CPU by writing kernel-owned registers. Owed: esp32-wroom-smp silicon selftest (5.14), the shared kernel on both LX6 cores taking the compare-and-swap from both and reading each core's processor identity; `docs/reference/boards.md` names its capture |
| `f411disco/f411spi` | 2 | the worked example of hand-rolled bring-up, rewritten as one: SPI1's window and line, `pinmux` for its pins; no claim by number, no task created by hand |
| `frdmk64f/k64console` | 2 | `stdout` names the packaged `k64uartirq` |
| `frdmk64f/k64drv` | 2 | retargeted from PIT channel 2 to LPTMR0, since PIT's AIPS slot 55 also holds the channels the kernel's time base chains and is never opened: LPTMR0 sits alone in AIPS0 slot 64 at 0x4004_0000, raises line 58, and is clocked by SCGC5 bit 0 (K64 RM 4.5.2 Table 4-2, 3.2.2.3 Table 3-5, 12.2.12). Its window and line, `device_not_isolated`, `coarse_gate` for its slot; the chip file gains the device, and `arch_periph_enable` the base, gating its clock and clearing its slot's SP bit as it does for UART0 and DSPI0. The driver counts the 1 kHz LPO (PSR PCS=01, PBYP=1; RM 42.3.2) and a thread of its task holding no window reads the counter, which succeeds, the slot being the gate: the window, 32 bytes by the SYSMPU's granule, already holds every register LPTMR0 implements, and the slot answers any other address with a transfer error (RM 4.5) |
| `frdmk64f/k64dspi` | 2 | two compositions, chosen by `KICKOS_SPI_LOCAL_ENGINE`: the packaged `k64dspi`, whose start muxes the pins the board file wires to DSPI0, and a client using its endpoint `/svc/spi0`; and a task holding DSPI0 and `pinmux` that links the local SPI engine and muxes those pins through `k64dspi_bus_mux` |
| `rx72m/rxdrv` | 2 | the port window, `pinmux`; the ungranted poke is a child thread's fault. The chip file's port window holds `PMR`, so it takes `coarse_gate` beside the console's pins and the kernel's LED, and the poke writes the console pin's `PB1PFS` in the MPC, which the chip file gives the kernel, instead |
| `xmc4800-relax/conreclaim` | 3 | retargeted: `testusic`, a test-owned console driver over USIC0 CH0 in `tests/drivers`, is named `stdout` and holds the console device alone, so no plain task may hold it. It serves plain sends polled, a zero-length one as a flush, and a zero-length call by scrambling the channel it holds, its clock gated last, before it answers; `main` prints through it, asks for the scramble and panics, and the verdict reaches the wire only through the kernel reclaiming the published console. Built only where `KICKOS_TEST_DRIVERS` is on, which the board preset leaves off, so an operator configures it in (`EXTRA_CMAKE=-DKICKOS_TEST_DRIVERS=ON`); `tests/integration/check_conreclaim.sh` judges the capture |
| `xmc4800-relax/consoledemo` | 2 | `stdout` names the packaged `xmcuartirq` |
| `inprstorm`, `pvprobe`, `xmccshold`, `xmcspi` | 2 | USIC0 CH1, and for `xmcspi` the line `/dev/usic0/sr1`; `inprstorm`'s entry holds the window and re-delegates it to a storm thread below itself, so it takes `memory` and runs on `ends: never`; `pvprobe` and `xmcspi` fault their own entry on the closing ungranted read, which ends the system, while `xmccshold` returns. Their compositions do not name `xmcssc`, so nothing else holds the window. The refusal of the old conflict is already armed: `tools/compose/tests/test_arms.py` grants each beside the golden system's `xmcssc` and reddens `ownership.device`, and `ownership.line` for `xmcspi` |
| `xmc4800-relax/xmcssc` | 2 | two images in every configure: `xmcssc`, the packaged `xmcssc` and a client using its endpoint `/svc/spi0`, and `xmcssc_local`, a task holding CH1 and its line that links the local SPI engine |

### 4.4 The selftest as a task

The selftest is `main` of its own composition on every board, `user/apps/common/selftest/system.yaml`,
holding `memory`, `system`, `pinmux`, `irq`, `console` and `tasks`, the authority it gave itself
through the app authority macro, with `ends: main` and a `heap` of 0: no arm allocates from the libc
heap, and the 64 KiB parts carve none. Its entry, `selftest_main`, keeps its own row of the table,
whose priority, ceiling, authority and delegations the arms read. A second composition per
console driver a board has, `consoles/<board>/<driver>.yaml` with `stdout` naming that driver,
replaces the service lists the bench fleet ran it under (4.5), its images named
`selftest_<driver>`; `k64uartirq`, `xmcuartirq`, `f4uartirq`, `lx6uart`,
`c6uart`, `rxsci`, the USB device consoles `rpusb` and `rt1062usb`, and `simcon`.

**On an AMP node** the composition names the node's crossings. On an own-image node it is
`systems/<board>/<variant>/node<k>.yaml`: `main` serves the first port the partition names the
node and uses every port it names another, and maps the partition's user share through a
partition region. It is admitted in a partition whose other nodes run `ampecho`, itself composed
(`echo<k>.yaml`), echoing on its node's first port while a task that never receives serves each
other one, which is the unanswered port the reply-guard arms park on; `amp_partition_selftest`
assembles that partition. The shared image's one composition names its crossings alone (9.1). An
AMP node builds no driver images.

**Priority.** Every `entry` task states its `ceiling`, which no default supplies, and the init
narrows the task's ceiling to it. The selftest spawns threads above the one it runs at, so it runs
at today's root priority, 2, under a `ceiling` above.

| composition | the console driver | the selftest's `ceiling` |
| --- | --- | --- |
| `stdout: kernel` | none | `KICKOS_PRIO_MAX` |
| `stdout` names a driver | `KICKOS_PRIO_MAX` less 1, its IRQ thread at offset 1 reaching `KICKOS_PRIO_MAX` | `KICKOS_PRIO_MAX` less 1, so no writer is above the console's receiver |

The ceiling arms read the declared ceiling rather than a constant, so one arm holds under both.

**Arms that read root, and what they read instead:**

| arms | today | as a task |
| --- | --- | --- |
| `authority_cap`, `task_authority`, `console_publish_priv` | root holds every authority | the task holds exactly its composition's word, which the arms assert: every bit of it seats on a child, `pstate` does not, and `cpu_clock_set` from `main` is refused |
| `console_publish_handout`, `console_publish_narrow` | root publishes its own endpoint | unchanged under `console` authority; skipped where `stdout` names a driver, as today under a service list |
| `thread_join`, `join_stale_gen`, `task_handles`, `call_from_root`, `thread_slay_gate` | root's thread 0, implicit task handle and slot layout | the task's own, read from `kos_thread_self`, which answers on every kernel |
| `cap_child_width` | root's summed width | the task's table is a child's (section 3), which the arm asserts, beside the init's delegations |
| `prio_self_ceiling`, `prio_ceiling_*`, `task_group_kill`, the SMP restore | root's priority and ceiling | the declared priority and ceiling |
| the IRQ arms | root claims `KICKOS_IRQ_FREE_BASE` lines | the task claims them at run time under `irq` |
| `caller_stack`, `domain_share`, `cross_task_block`, `window_get` and the region-set reads | root's region set | the task's, on the same static regions |
| `stack_grant_refused`, `stack_handoff_refused` | root's stack, which the kernel placed | a worker's, the task's own stack being a block the init reserved and handed it |
| `amp_probe_root_only`, now `amp_crossing_task_local` | root holds the partition's ports | the task holds the crossings its composition names, and their indices name nothing in another task's table |
| `amp_port_seating`, `amp_port_unnamed` | the ports at root's seated indices | each named crossing at its delegated slot, and none other |
| `amp_share_window`, `amp_share_crossing` | root holds the user share and hands windows of it | the task reaches the share through its composition's partition region, and naming any part of it itself is refused |
| `process_data_from_image`, `irq_as_event` | root's data pages | the task's: a task a spawn creates copies its spawner's live data, consistent on more than one core only while the spawner's other threads write no static data across the spawn, an explicit task the snapshot |
| an arm reading a pin a board pin map set | the default init applied it | none exists |

The pools follow the task. A task's entry thread and task come out of `KICKOS_MAX_THREADS` and
`KICKOS_MAX_TASKS`, raised by one on `microbit`, `f302nucleo` and `bluepill-c8` (section 3), and its
table is a child's: a self-test defconfig states `KICKOS_MAX_SPAWN_GRANTS=9` on a board whose
supply backs the 3 optional slots, and an AMP node's defconfig widens it for its crossings.

**Where the selftest overflows an image under its composition, it splits into one more region**,
as ruled. `main.cc` cuts ten regions, and each board states the first region of each of its
images: ten on the two STM32 parts, one region each; five on `microbit`, whose binding resource is
the arena after `.bss`; one on an enforcing XMC4800, whose arena spans DSRAM2; five on an
enforcing ESP32-C6 bench build and four on the other enforcing or AMP builds of that chip; two on
every ESP32 build. Its bloat audit stays
in M10.6.

### 4.5 Every service list becomes a composition

| list | becomes |
| --- | --- |
| `kickos_services_none` | the default composition |
| `kickos_services_frdmk64f` (a polled console driver, since removed, and `k64dspi`) | `k64console`'s and `k64dspi`'s compositions |
| `kickos_services_xmc4800relax` (a polled console driver, since removed, and `xmcssc`), `_xmc4800relax_console` | `consoledemo`'s and `xmcssc`'s |
| each `_uartirq` list (`k64uartirq`, `xmcuartirq`, `c6uart`, `lx6uart`, `rxsci`, `f4uartirq`) | the selftest's composition naming that console driver on its board |
| the `_usbcdc` lists (`rpusb`, `rt1062usb`) | `usbcdcwit`'s compositions and the selftest's composition naming that console driver on its board |
| `kickos_services_sim` (`simcon`), `kickos_services_simuart` | the compositions of `drvdeath`, `pubpanic`, `simconabi`, `faultsurvive_published`, the published selftest, and `uartloop` |

The catalogue gains every driver a list carried that it lacks: `k64dspi`, `simcon`, `simuart`,
`rpusb` and `rt1062usb`, each declared on `kickos_add_driver` with its roles, threads and `START`. The local SPI engines and `i2c_rxriic` stay libraries a task links.

### 4.6 Images under `tests/`

The composition, driver and console witnesses and the system-link probes already compose. Their
x86_64 branch becomes `add_executable` with every other image (section 7).
`tests/lib/package_names` loses its `kickos_cxx` and mixed probes, and `tests/composition`
compiles against `KickOS::kernel`.

## 5. Packaged drivers take their lines from the composition

The instance becomes the whole of what `START` is given. `struct kos_driver_instance` carries the
task's name and priority, each window role's base and size, each line as its number and its
index within its device, the core mask, whether the table names it the console driver, the badged
copy of this start, the endpoint, the ring block and the task handle the bring-up writes back.
`kos_service_cfg` and `kos_svc_kind` go with the service lists that were their last other reader
(`TODO.md`, "WHAT M10 DELETES").

A driver's `Descriptor` keeps each line's role and trigger and loses its number. `bring_up`
claims the instance's lines at the descriptor's trigger; its instance check keeps the count. A
driver that programs which line its device raises, as a USIC channel's INPR does, programs the
line's index within its device.

| driver | its number today | the composition binds |
| --- | --- | --- |
| `xmcuartirq` | 84, `USIC0_SR0` | a USIC0 line, `/dev/usic0/sr0` in the selftest's driver composition |
| `xmcssc` | 85 | `/dev/usic0/sr1`, as the golden system does |
| `f4uartirq` | 38, `USART2_IRQ` | `/dev/usart2/global` |
| `k64uartirq` | 31, `UART0_RXTX_IRQ` | `/dev/uart0/rxtx` |
| `c6uart` | 16, `UART0_TX_LINE` | `/dev/uart0/uart0` |
| `lx6uart` | 30, `CONSOLE_TX_LINE` | the console UART's line in the ESP32's chip file |
| `rxsci` | 86 and 87 | the console SCI's receive and transmit lines in the RX72M's chip file |
| `rpusb` | 5 on the RP2040, 14 on the RP2350 | the USB controller's line in each chip file |
| `rt1062usb` | 113, `USB_OTG1_IRQ` | the USB OTG1 controller's line in the i.MX RT's chip file |
| `simuart` | 29 | the sim chip file's UART line |
| `k64dspi`, `simcon` | none | none: lineless |

**In two steps.** M10.5.7 lands the catalogue, the instance carrying each line's number and index,
and `bring_up` given an instance claiming the instance's lines, so every composed driver already
takes its lines from the composition while the service lists still run. The deletions row then
removes, with the service lists, the descriptor's numbers (`Descriptor::Line::number`), the line
argument of `KICKOS_UART_CONSOLE_SERVICE`, `bring_up` given no instance with the posture bit a
driver thread reads to choose between trapping and panicking, and `kos_service_cfg`, `START`
taking the instance alone. Every failing driver thread then traps.

## 6. The deletions

A row lands only once nothing composes a system through it: after the app and selftest rows and
x86_64's move to `add_executable`.

| mechanism | replaced by | gates, tests and docs that change |
| --- | --- | --- |
| the service lists: every `system/init/<board>/service_list*.cc`, `services_none.cc`, `service_list_run.cc`, `kos_service_list`, `kos_service_bringup`, `kickos_board_services`, `kickos_service_list_run`, the service-list selection knob and the two board defaults | compositions (4.5) | `check_service_lists.sh` and `service_lists.txt` deleted; `kconfig_gen`'s string arm moves to another string knob; the `check_sim_*.sh` gates configure compositions instead of `-D` lists; `boards.md`, `architecture.md`, `invariants.md`, `porting.md` |
| `kos_service_cfg`, `kos_svc_kind`, the descriptor's line numbers, the line argument of `KICKOS_UART_CONSOLE_SERVICE`, `bring_up` given no instance and the posture bit | `START` taking the instance (section 5) | `tests/unit/drvbringup/bringup_unwind.cc`'s number-mismatch and service-list cases; the size assert on the cfg |
| `kickos_add_board_provider` and `cmake/cap_table.cmake`'s summing, with `kickos_declare_app_capabilities` and `kickos_declare_app_endpoints` | root, which is the init, gets a table `KICKOS_CAP_TABLE_SUPPLY` wide, the supply admission already counts the init against; the init's endpoints are admission's `supply.budget` | the selftest gate's partials on the summed width; `architecture.md` |
| the pin maps, the pin-map selection knob, `pinmap.h`, `pinmux_run.cc` | a task muxes its own pins under `pinmux`; the board file states the wiring | `check_provider_alias.sh` and its fixture; `boards.md` |
| the default init: `kickos_default_init`, the init-provider cache variable, the app authority macro, its default definition's source file | the composed init, authority per task | the `init_flush` host test is deleted, the walk's host tests holding the stdout drain at the ending; `check_seam_defaults.sh` loses those archives |
| `kickos_root_lower` | `kickos_root_entry` runs the constructors and calls the init, which lowers itself | the link group and `kernel/init/kmain.cc`'s reference |
| the old leaves `kickos` and `kickos_cxx`, `KickOS::kickos`, `KickOS::kickos_cxx`, and the leaf-mixing refusal | `KickOS::kernel` alone | `check_oot_export_mcu.sh`'s probes; `tests/lib/package_names` |
| `KICKOS_USER_HEAP_SIZE` the knob: Kconfig, three board overrides, the defconfigs, the leaves' `--defsym`, x86's knob path | every composition's `heap` (maintainer, 2026-10-03); the link symbol of that name stays, defined by the system target alone | `cmake/kernel_leaf.ld`'s fallback; `check_heap_symbol.cmake`; `porting.md`, `boards.md` |
| `kos_wait_last` and its system call, root-only | `ends`, and the task end that stops a task's members | `sched_exit`'s arm; `abi.h`'s row |
| the service-list sweep tool | the fleet sweep over every preset | `boards.md` |
| the bench's `SERVICE_LIST`, and `bench-fleet.sh`'s list discovery | the bench names an image, and the fleet runs each selftest composition of the board as its own image | `tools/bench/bench.sh`, `tools/bench/bench-fleet.sh`; the USB console's clock and capture route read from the image (2.1) |
| the hand-written chip headers, `mpu.cmake`, `aspace.cmake`, the reserved arrays and their fixed bound | section 1 | the configure-time pair checks; `porting.md`'s chip-header steps |
| the `KickOS::system_default` stub | every board's default (section 2) | the `system_link_stub` gates |

Order: the service lists, the service ABI and the drivers' numbers, the bench's lists and the
sweep tool go together, then the default init with the pin maps and the app authority macro, then
`kickos_root_lower`, then the cap-table summing, then the heap knob, then the root-only wait,
then the old leaves last, since every earlier deletion leaves them nothing to carry. The chip
headers and the stub go as section 1 and section 2 land.

## 7. x86_64 through `add_executable`

This lands right after the board files and before any app moves onto a composition, so the old
leaves `kickos` and `kickos_cxx` carry the same x86_64 usage requirements as `KickOS::kernel` until
they are deleted, and no app's CMakeLists is rewritten twice.

The installed `cmake/toolchain-x86_64-uefi.cmake` sets the executable link rule of C and C++ to a
wrapper, with `.efi` as each language's executable suffix. The per-language suffix is the one that
holds: CMake's generic platform resets the plain one after the toolchain file is read.

```cmake
foreach(_kos_lang C CXX)
  set(CMAKE_EXECUTABLE_SUFFIX_${_kos_lang} ".efi")
  set(CMAKE_${_kos_lang}_LINK_EXECUTABLE
      "\"${_kos_link}\" \"${KICKOS_X86_64_LD}\" \"${CMAKE_READELF}\" \"${CMAKE_OBJDUMP}\" \"${CMAKE_OBJCOPY}\"${KICKOS_X86_64_LIBDIRS} <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
endforeach()
```

`_kos_link` is the tool beside the toolchain file, and `KICKOS_X86_64_LIBDIRS` the `-L` directory
of each toolchain library, which the tool resolves every `-l` against and hands `ld` as a path.

**The wrapper**, installed beside `tools/x86_64-krel.sh`, runs what the image's custom command ran,
in order: `tools/check-x86_64-no-got.sh` and `tools/check-x86_64-weak-undef.sh` over the
objects and every archive, the first `ld -m i386pep` pass, the relocation directory's extract, its
object, the second pass, and the check that the directory did not move. It passes
`-b elf64-x86-64` unconditionally, and CI's probe of whether that flag is load-bearing stays;
CI links with the toolchain's binutils 2.47, so the 2.42 path the flag exists for is not
witnessed. It writes the map beside the image as `<TARGET>.map`, the path the gates read through
`KICKOS_IMAGE_MAP`, and a failed link leaves neither the image nor the map.

- **The fleet keeps its `-Wl,` spelling.** The wrapper turns each `-Wl,a,b` it is given into `a b`
  for `ld`, so the kernel target's link options, the `--require-defined` of the system-target
  symbol, the system target's heap `--defsym`, a map request and the link group's
  `--start-group`/`--end-group` reach the linker unchanged. It refuses an argument it does not know
  rather than pass a compiler-driver flag to `ld`.
- **The boot and kernel-landing objects ride usage requirements**: every leaf links the boot
  OBJECT library for its core count and the landed kernel's, so `$<TARGET_OBJECTS>` places them;
  `libc`, `libm`, `libgcc`, `libstdc++` and `libsupc++` are named by `-l` inside each leaf's
  group, the toolchain file's rule carrying their `-L` directories, so no path of the building
  machine reaches the exported targets; `-T` names `pe_image.ld`. The order CMake gives the
  objects lays out the app half and nothing depends on it.
- **A rebased image** is a second executable over the first's objects (`$<TARGET_OBJECTS:>` of
  the first) and its link libraries, linked with `-Wl,--image-base=<base>`, so nothing is
  compiled twice.
- **The boot probes** in `cmake/x86_64_boot.cmake` keep their own single-pass command, through
  the same tool's `--one-pass`: they are bring-up images, not apps.
- **The configure's own compiler checks** stay static libraries, as
  `kickos_toolchain_bare_metal_rules` already sets them, so they never reach the wrapper.
- **What goes**: `kickos_add_app_target`, the OBJECT branch, `kickos_x86_64_link_image` and its
  deferral to the end of the configure, the system targets' `KICKOS_SYSTEM_*` properties that
  exist only for it, and the x86 groups `KickOSConfig.cmake` bakes. `kickos_emit_image` keeps
  only its mixed-leaf refusal on x86_64. An app calling a driver class names it through
  `kickos_link_class_backends`, and `user/apps` applies the tree's warning and C-standard posture
  at directory scope.

Proven on q35 at one to twelve cores under the composition and x86 gates, with every loaded
section of every image byte-identical to the custom-command link it replaced, and installed:
`examples/oot-mcu-app` links through it.

## 8. The out-of-tree examples

`examples/oot-app` (the sim) and `examples/oot-mcu-app` link `KickOS::kernel` and
`KickOS::system_default`, `add_executable` on every arch once section 7 lands. Their gates,
`check_oot_export.sh` and `check_oot_export_mcu.sh` over `tests/integration/oot_arch_boards.txt`,
build them against the installed package. The sim gate also runs its app, and on a board an
emulator runs, the image gate `check_oot_mcu_run.sh` boots the MCU app. Each run expects the app's
line and its `main`'s status through the default composition.
The configure of every board refuses an arch with no row in that map. `examples/composition` is kept correct with the
tree like any track: the golden gates build and run it, and the Relax Kit captures judge its lines.

## 9. The partition build

### 9.1 Admitted together, on every node

An AMP app states its partition once:

```cmake
kickos_compose(app_system PARTITION systems/node0.yaml systems/node1.yaml)
```

Each node's build takes the composition at its own node index, which its manifest states, and
admits **every** node's composition together. A peer is configured from node 0's `.config` with
only the node index changed, which `tools/amp/build-partition.sh` already checks by fingerprint,
so node k's composition is admitted against this build's manifest with its node set to k, exactly.
Every node refuses alike, and no node waits on another's build. The tool runs it as
`kickos_compose partition <composition>... --manifest <manifest> --node <k>`.

**A shared image is its partition's one composition.** Its peers run a service body and no
kernel, so nothing runs a composition there: the image links node 0's, which names its crossings
alone, the manifest's `target.amp.image` stating `shared` (`own` otherwise) so that admission does
not refuse it as `partition.lone`.

### 9.2 What crosses, and how it is named

A crossing is a port of the partition, which Kconfig states (`KICKOS_AMP_PORTS`). A composition
names it as an endpoint under `/amp`: `serves: /amp/<port>` on the node the partition names as its
server, `uses: /amp/<port>` on the others. A crossing orders nothing: the far server's readiness
is a fact of another node. The init delegates the port the kernel seated in root, with WAIT to
its server and SIGNAL to its users; the lookup is `kos_grant_endpoint`. Port 1 is never named;
port 0, the window layer's echo, may be used.

A **partition region** is a `shared` entry with `partition: true`, mapped by every node whose
composition names it. It lives in the partition's user share, which the kernel share of 9.3
provides; the partition build places each region in it in the order node 0 declares them, each
node's table carries the region's offset, and the init maps the region at its offset rather than
reserving it.

### 9.3 The partition's kernel share

A partition region needs the kernel first, each change with its arm, in a row before the
partition build:

| change | arm |
| --- | --- |
| Kconfig gives the partition a user share, carved from the AMP shared window after the kernel's own use, its size exported in the manifest's `target` | a configure refusing a share past the window |
| the kernel seats the share in root as a reservation at boot on every node, as it seats the ports, so the init can map from it | the selftest reads the reservation's base and size on node 0 of `qemu-arm64-amp2`, and on node 1 booted alone; inside the partition node 1's seat is witnessed through the crossing, its echo writing through a self-grant of the share |
| the grant path's confinement of memory to the arena admits a window inside the share for the holder of its reservation, and nothing else outside the arena | a window one byte past the share refused; one inside it from a task holding no reservation refused |
| on a translating node the share maps into a task's space as any window does | a write through one node's mapping read back through the other's |
| the share has ONE memory type, a fact of the whole partition, stated in Kconfig and exported in the manifest beside its size: the kernel maps the share with it in its own view too, and a window or grant over the share asking the other type is refused, so no two mappings of it disagree; a partition region's `cache` must be the share's (the compose rule is the partition build's) | the uncached share's type read back by the hardware's walk in the kernel's view and a task's on `qemu-arm64-amp2`, the cached share's on `qemu-arm64-amp3`; a window of the other type refused on each |

### 9.4 The rules, each with its arm

| rule | refuses | arm |
| --- | --- | --- |
| `partition.device` | a device, or a gate around it, two nodes grant | two node compositions granting `/dev/rtc` on `qemu-arm64`, in the tool's tests |
| `partition.port` | an `/amp` port the manifest does not list or lists for two nodes, port 1, a `serves` on a node not its server, a port two tasks of one node serve, a task that serves and uses one port, a packaged driver serving one | a mutated node composition each |
| `partition.unserved` | an `/amp` port a node uses that no task of its server serves | the server's `serves` removed |
| `partition.region` | a partition region two nodes declare with different sizes, one another node declares without `partition: true`, one whose `cache` is not the share's one type (so two nodes never differ in it), the regions past the user share, or one where the kernel build states no AMP shared window | a size changed on one node; `partition: true` dropped on node 1, on node 0, and on two nodes of three, and dropped on both nodes admitted; a `cache` changed on one; a size past the share on both; a node composition emitted alone against a build stating no window |
| `partition.lone` | on an AMP build, a crossing or a partition region in a composition admitted apart from its partition, whose system target no other node's admission sees | a node composition emitted alone against node 0's manifest, once holding its region and once its crossing |
| `partition.cached-incoherent` | a `cached` partition region shared by nodes that are not coherent, unless every node that maps it accepts `cached_incoherent` | the i.MX 8M Plus's clusters as a two-node fixture, refused, admitted with the acceptance on both, refused with it on one |
| `partition.gate-budget` | a gate's derived assignment past the regions it holds, a device granted where a per-peripheral gate states no register for it or no region gate's `ranges` front it, and one register two nodes' grants would each be assigned | the C6's LP node granted the LP UART, which coalesces with none of the LP kernel's pages; the C6's HP_APM cut to two regions, refused by the partition, by a node composition holding no crossing and no region emitted alone, and by the assignment of no composition; the RP2350's UART0 with its register removed; the C6's HP_APM ranges narrowed past `timg1`, granted on node 1; two UART0 instances under one register, granted on two nodes |

Nodes are coherent when they run on one cluster the chip file states `smp`, or where no data cache
sits over the region (`data_cache: false`, as the C6's file states), where both cache values are
admitted.

### 9.5 The partition gate

The assignment is derived: each device's node is the node whose composition grants it, so no file
can disagree with the compositions. Every system target built for node 0 of an AMP build, and every
one built on a chip stating a gate, emits it as one C source, empty where the chip states no gate,
and an image linking no system target links the assignment of no composition, the kernel's own
rows alone, so the gate's owner reads a definition in every link that programs it and no weak
symbol stands in for one. The chip code programs exactly the emitted assignment, and the literals
it programmed move into it. Which gate fronts a device is stated once, by the gate's `ranges` in
the chip file, a window crossing a range's edge being `chip.gate-straddle`; a per-peripheral gate
names each device's register as the device's `gate_register`.

**The ESP32-C6's APM** (TRM v1.2 chapter 16). The chip file states two gates. HP_APM holds 16
regions and fronts the HP peripherals (0x6000_0000 to 0x600A_FFFF), the CPU peripherals
(0x600C_0000 to 0x600C_FFFF) and HP SRAM as the LP CPU reaches it. LP_APM holds 4 and fronts the LP
peripherals (0x600B_0000 to 0x600B_FFFF) and LP SRAM in low-speed mode (Table 5.3-2, Figure
16.3-1). Region 0 of each is the reset catch-all that denies, permissions OR across regions, and a
denial reads zero and drops a write without a trap (16.3.2.3, 16.5). A permission is per security
mode and never per master: the HP CPU's user mode is REE0, and the LP CPU is REE2 at reset
(16.3.1, Reg 16.56), which every other bus master shares at power-up (the note of 16.3.2, p.564),
so node 0 moves the LP CPU to REE1 (`LP_TEE_M0_MODE_CTRL`) before it wakes it and no DMA master
holds what the LP node is given. The derived set per gate, each range coalesced with the ranges it
touches, counted against the gate's regions less its catch-all:

| gate | REE0, the HP node's tasks | REE1, the LP node | kernel ranges kept in the set |
| --- | --- | --- | --- |
| HP_APM | each HP device the HP node's composition grants, and nothing else: none where an image links no composition; no CPU peripheral, which no composition grants and among which INTPRI is the kernel's doorbell | the LP node's slice and the shared window in HP SRAM, and each HP device its composition grants | the slice and window rows |
| LP_APM | each LP device the HP node's composition grants | the LP kernel's own pages (the PMU page, its reset vector, the LP timer page) and each LP device its composition grants | the LP kernel's three pages, which are the derived set before any grant |

So an LP grant is admitted only where it coalesces with one of the LP kernel's three pages, and
refused otherwise: the LP kernel's pages are part of the derived set rather than fixed regions the
grants must fit around, and none is dropped to make room. As the pages stand, none coalesces: the
LP UART, which the chip file gains from Table 5.3-2, sits past LP_AON, which is the kernel's, so
every LP peripheral granted to the LP node is refused rather than taking a region from the kernel.
With no grant, HP_APM holds the LP node's two REE1 rows alone, leaving thirteen. Each gate's
FILTER_EN is written whole, so a region no row names stays disabled whatever enabled it before;
`c6lpprobe`, which programs HP_APM's regions 14 and 15 and every LP_APM region past the catch-all
itself, refuses an image whose rows hold any of them.

The literals the chip code programmed before the derivation opened more than this set: REE0 held
read and write on RMT and IO_MUX, which are the kernel's, on HP SRAM and on every CPU peripheral
including INTPRI, and the LP node's slice and window were REE2's, so every DMA master reached them.
The LP reset-vector row stays at 0x7000_0000, which Table 5.3-1 leaves reserved: LP_APM sees the
LP CPU's reset fetch from LP SRAM (0x5000_0000) at that alias, as the archived probe capture
archived `M9.9_lp_probe_captures/m99probe12.log` shows, and the chip file cites it.

**The RP2350's ACCESSCTRL** (RP2350 datasheet, 10.6). One register per peripheral at 0x4006_0000
plus the offsets of Table 911. Node 0, Secure and privileged, writes each before it launches the
peer, with 0xACCE in bits 31:16 of every write, a write without it bus-faulting (10.6, p.823). For
each peripheral a node's composition grants, the register image is:

| bits | value |
| --- | --- |
| 7, DBG | 1, as at reset, so the debugger keeps reaching it |
| 6, DMA | as at reset; the DMA controller is no composition's to grant while DMA is deferred, `bus_master` being refused unless accepted |
| 5, CORE1 and 4, CORE0 | the granting node's core 1 and the other core 0 |
| 3, SP and 2, SU | 1 and 1: the kernel is Secure privileged, its tasks Secure unprivileged (10.6.2, pp.824-825) |
| 1, NSP and 0, NSU | 0: no Non-secure world runs |

A peripheral no composition grants keeps its reset value, and **LOCK is never written**, since a
lock bit holds until reset (Table 912, p.830). **Never assigned**, because both kernels need them
or they are not per-peripheral at all: SIO, the PPB, BOOTRAM and ACCESSCTRL itself, which no
register gates (10.6.2.3, p.826); ROM and the boot path through which node 0 launches the peer;
XIP_MAIN, XIP_CTRL, XIP_QMI and XIP_AUX, both images running from flash; SRAM0 to SRAM7, whose
banks 0 to 3 and 4 to 7 are word-striped (2.2.3, p.31), so a bank is never one node's range, and
SRAM8 and SRAM9, which are not striped and hold the cores' stacks; DMA, deferred;
RESETS, CLOCKS, XOSC, PLL_SYS, PLL_USB, TICKS and WATCHDOG; TIMER0, which both kernels read; UART1,
the console both nodes write under their claim; and IO_BANK0 with PADS_BANK0, whose pins the
consoles and the boot share. The chip file's `never_assigned` names each of their registers, and
the tool refuses a device whose `gate_register` it names, or one two devices name
(`chip.gate-register`); node 0 refuses the same before it writes any register, a row naming a
register of that list or one an earlier row names. Each the chip file describes is
`owner: kernel` but UART1, the board's console, which a kernel `stdout` keeps from any task
(`ownership.console`); the rest are no device of it at all. A blocked access is a bus error and so
a fault, unlike the APM's (10.6.2, p.824).

**The i.MX 8M Plus's RDC** has no consumer: no M7 image is built, so no partition of the part
exists to program. The A53 `virt` machine states no gate and emits none.

**Witnessed on silicon**:

- `esp32c6-wroom-amp2`: `ampping`'s LP task reads TIMG_NTIMERS_DATE (0xF8, TRM Register 14.25)
  through the timer group its composition grants, TIMG1, nonzero, and from TIMG0, which the HP
  node's composition grants, zero, a denial that does not trap; node 0 reads its own TIMG0
  nonzero. Judged by `tests/integration/check_c6_amp_capture.py`, through
  `tests/integration/check_c6_amp_gate.sh` as `bench.sh`'s `JUDGE`.
- `pizero2350-amp2`, when the bench holds the board: node 0 reads back UART0's ACCESSCTRL register
  as its own core's alone, its task holds UART0, enables it through `kos_periph_enable`, which the
  RP2350 gains for UART0's reset bit, and reads UARTPERIPHID0 at offset 0xFE0, 0x11 on a PL011,
  judged by `tests/integration/check_pizero_amp_gate.sh`. Node 1's own refusal of that register
  is not witnessed: no composition grants the privileged thread the read needs, and the node hook
  that would make it is owed (TODO.md, M10.6.6).

The partition region and the crossings run in CI on `qemu-arm64-amp2`, `ampping`'s nodes calling
across and exchanging through one region.

## 10. The silicon witnesses

The bench chain knows a board by its rows in `tools/bench/board-rows.sh` and its capture arm in
`tools/bench/bench-capture.sh`; `tools/bench/bench-present.sh` attests the boards a probe row
identifies. Each run is judged by a script over the capture, through `bench.sh`'s `JUDGE`:
`check_golden_system.sh`, `check_composition_witness.sh`, `check_system_default.sh` reading
`KOS_CAPTURE` as the first two do and named by the `sysdefault` image, the selftest's TAP verdict as
the fleet reads it, and a judge per board app that prints.

| board | the chain has | runs |
| --- | --- | --- |
| `xmc4800-relax` | probe, console, fleet | default system; selftest, kernel console and `xmcuartirq`; the golden system and the restart witness; `consoledemo`, `xmcssc` in both forms (`xmcssc`, `xmcssc_local`), the four diagnostic apps, `conreclaim` |
| `frdmk64f` | probe, console, fleet | default system; selftest, kernel and `k64uartirq`; `k64console`, `k64drv` on LPTMR0, `k64dspi`'s LAN9252 `BYTE_TEST` |
| `rx72m` | probe, console, fleet | default system; selftest, kernel and `rxsci`; `rxdrv` |
| `f302nucleo` | probe, console, fleet | default system; selftest |
| `esp32c6-wroom` | probe, console, fleet | default system; selftest, kernel and `c6uart`; `c6blink`; from the flat build, `c6lpprobe` and `c6txidle`; the `amp2` partition with its gate witness |
| `esp32-wroom` | probe, console, fleet | default system; selftest, kernel and `lx6uart`, one and two cores |
| `f411disco` | probe, console, fleet | default system; selftest, kernel and `f4uartirq`; `f411spi` |
| `picopi`, `pizero2350`, `teensy41` | console and capture, no probe row | default system, selftest, `usbcdcwit`; `pizero2350-amp2`'s `ampping` and gate witness |

**No bench row**: `microbit` (QEMU runs it in CI instead), `due`, `bluepill-c8` and `blackpill`
(their probe is shared and undecidable, and no console row exists), and `imx8mp-evk` (not
flashed). Those are witnessed by their links in CI alone, and the report says so.

## 11. What M10.5 leaves, and its ABI touchpoints

**To M10.7**: the exit record and the reference documents reconciled against what shipped. M10.5
updates every page that names what it deletes, since the doc-name gate requires it, and
`design-m10-composition.md`'s schema for every field it adds: the `mprotect` unit, `host` devices,
the baseless arena, `blocks`, `link`, `interrupts`, `cycle_counter`, `c`, `symbol`, `manual`,
`ref`, the partition gate's list, `ranges`, `regions` and registers, the `/amp` namespace,
partition regions and the `port` grant kind. **To M10.6**: the selftest's bloat audit and the
ESP32's code space, unless the selftest's composition overflows an ESP32 image, which a further
region answers first. **Not assigned anywhere**: the i.MX 8M Plus's M7 port and the RDC
programming it would need, DMA, and the boot format.

**The kernel's share of M10.5** is section 9.3's (the partition's user share, its seat in root,
the confinement exception, the translating mapping and the cache attribute), the span API over
the generated tables, the RP2350's and the K64F's `kos_periph_enable` bases, and the deletion of
the root-only wait.

```c
/* user/include/kickos/sys/table.h, KICKOS_TABLE_VERSION 6 */
uint8_t ceiling;             /* kos_table_task, the reserved byte after driver: the priority
                                ceiling the init grants the task */
KOS_GRANT_PORT = 8,          /* an /amp crossing: flags carry serve (WAIT) or use (SIGNAL) */
uint32_t offset;             /* kos_table_region, growing it to 16 bytes: a partition region's
                                offset in the partition's user share */
KOS_TABLE_REGION_PARTITION   /* kos_table_region flags */

/* user/include/kickos/sys/driver_service.h */
struct kos_driver_line { uint16_t number; uint16_t index; };
struct kos_driver_instance;  /* gains name, priority, the window roles' bases and sizes, the
                                lines as kos_driver_line, the core mask, the console flag and the
                                badged copy: START's only argument */
/* Descriptor::Line loses `number`; KICKOS_UART_CONSOLE_SERVICE loses its line argument */

/* system/include/kickos/sys/service.h: deleted with kos_service_cfg, kos_svc_kind,
   kos_service_list, kos_service_bringup and kickos_board_services */
/* system/include/kickos/sys/init.h: kickos_service_list_run, kickos_default_init_run,
   kickos_root_lower and KICKOS_APP_AUTHORITY deleted */
/* system/include/kickos/sys/pinmap.h: deleted */

/* user/include/kickos/sys.h, user/include/kickos/sys/abi.h */
int kos_wait_last(void);     /* deleted, with its system call */

/* arch/include/kickos/arch/arch.h: the three table seams answer a span (section 1.2) */

/* emitted for every node-0 system target of an AMP build, read by the gate owner's chip code */
extern struct kickos_partition_gate const kickos_partition_gate;
```
