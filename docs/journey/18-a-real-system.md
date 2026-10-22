<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 18. A real system: an SPI bus service and its client

Chapters 01 to 09 each followed one mechanism on its own: the boot, a syscall, a call and its
reply, an interrupt reaching a driver, a spawn with its capabilities, a fault. This chapter puts
them together around one real driver and one real user of it. The driver is `k64dspi`, the SPI
bus service of the NXP FRDM-K64F board: an unprivileged task that owns the chip's DSPI0
controller and clocks bytes on the SPI bus for whoever asks. The user is a small application,
also called `k64dspi`, that reads a test register of a LAN9252 EtherCAT chip on the bus.

By the end you will have followed the system from the file that describes it to the bytes on the
wire and back: the composition that names the two tasks and their grants, the table the build
makes of it, the init starting the driver and then the client, the driver bringing the
controller up, the client opening the bus through the SPI class, one request framed, carried by
`kos_call`, run by the driver and answered. Then you will build the same client source the other
way, owning the controller itself, and see what changes. Last, you will read what happens when
either side fails.

The ideas are in three Book chapters:
[Synchronous call/reply](../book/synchronous-call-and-reply.md) (the service model: a class, and a
thread that speaks the wire), [The fast path is the
capability](../book/the-fast-path-is-the-capability-gpio-direct-mmio.md) (why the chip select is a
direct register store), and [Peripheral isolation and the hardware
ceiling](../book/peripheral-isolation-and-the-hardware-ceiling.md) (what the K64F can and cannot
isolate). The wire contract is [Reference: The SPI / I2C bus-service
contract](../reference/bus-service.md), and the transport is [Reference: Synchronous call/reply
IPC](../reference/ipc-call-reply.md).

## The system, and how to read it without a board

The pieces, all in the tree:

- **The composition** (`user/apps/frdmk64f/k64dspi/systems/proxy.yaml`): two tasks, the driver
  and the client.
- **The driver service** (`system/driver/mk64f/k64dspi/k64dspi.cc`): the start shim and the
  driver thread. It owns no register code of its own.
- **The engine** (`system/driver/mk64f/k64dspi/spi_dspi.cc`): the DSPI register work, written
  once against the SPI class.
- **The SPI class** (`user/include/kickos/driver/spi.h`): four functions, `kos_spi_bus_open`,
  `kos_spi_device_open`, `kos_spi_transfer`, `kos_spi_bus_close`. Every SPI backend implements
  exactly these.
- **The proxy** (`user/lib/spi_proxy/spi_proxy.cc`): a backend of the same class that turns each
  call into a message to the service.
- **The service loop** (`user/include/kickos/sys/spi_service.h`): `serve_loop` and `serve_one`, the
  part of the driver that speaks the wire.
- **The wire format** (`user/include/kickos/sys/bus.h`): the request and reply structs.
- **The client** (`user/apps/frdmk64f/k64dspi/main.cc`): `k64dspi_main`.

No emulator in this tree models the K64F, so nothing in this chapter was run under gdb. Two kinds
of Try-it follow. The static ones read the linked images with `arm-none-eabi-nm` and
`arm-none-eabi-objdump` from the KickOS toolchain, or run the composition tool on the host; they
were all checked on the two builds below, and the addresses come from them. The live ones need a
FRDM-K64F with a debugger attached, and are marked unverified.

`k64dspi` is built only on an enforcing build (its source stops with `#error` without one), so
the preset is `frdmk64f`, not `frdmk64f-flat`. The option
`KICKOS_SPI_LOCAL_ENGINE` picks the build: off (the default) is the service and its proxy client,
on is the client owning the controller (Stop 10).

```sh
cmake --preset frdmk64f -B build/frdmk64f
cmake --build build/frdmk64f --target k64dspi
cmake --preset frdmk64f -B build/frdmk64f-local -DKICKOS_SPI_LOCAL_ENGINE=ON
cmake --build build/frdmk64f-local --target k64dspi
```

The image is `user/apps/frdmk64f/k64dspi/k64dspi` under each build directory. The Try-its below
name the two images by shell variables:

```sh
PROXY=build/frdmk64f/user/apps/frdmk64f/k64dspi/k64dspi
LOCAL=build/frdmk64f-local/user/apps/frdmk64f/k64dspi/k64dspi
```

For a live session, flash and attach with the J-Link GDB server, as the
[flashing guide](../flashing.md) does for its J-Link boards, with the K64F's device name:

```sh
JLinkGDBServer -device MK64FN1M0xxx12 -if SWD -speed 4000        # terminal 1
gdb-multiarch $PROXY -ex 'target remote :2331' -ex load -ex 'monitor reset'   # terminal 2
```

---

### Stop 1: the composition names two tasks and what each may hold
**Where:** `user/apps/frdmk64f/k64dspi/systems/proxy.yaml`; `platform/mk64f/frdmk64f.yaml`
(`buses`); `platform/mk64f/chip.yaml` (`protection`, `devices`); `tools/compose/kickos_compose/composition.py`,
`admit_composition`, `check_enforcement`, `grant_limits`

A **composition** is the file that says what a system is: which tasks run, at which priority,
and what each one is given. The init starts exactly what it says, from a table the build makes
of it (Stop 2). This one has two tasks:

```yaml
tasks:
  - name: k64dspi
    driver: k64dspi
    devices: [/dev/dspi0]
    accepts: [coarse_gate, device_not_isolated]
    serves: /svc/spi0
    priority: 12

  - name: client
    entry: k64dspi_main
    priority: 9
    ceiling: 9
    uses: [/svc/spi0]
```

Read it line by line, because every grant in the rest of the chapter starts here.

- `driver: k64dspi` says this task is a **packaged driver**: the init does not spawn a thread at
  an entry point, it calls the driver's own start function, which the driver's build declaration
  names (Stop 2).
- `devices: [/dev/dspi0]` grants the driver the **register window** of DSPI0. The chip file gives
  that device its address and size: `dspi0: { window: [0x4002C000, 0x40], ... }`. A window is a
  range of device addresses a thread is allowed to reach.
- `serves: /svc/spi0` makes the driver the receiver of an **endpoint** called `/svc/spi0`, and
  `uses: [/svc/spi0]` gives the client the right to call it. An endpoint is the kernel object a
  call and its reply meet on ([05](05-call-and-reply.md)).
- The client gets nothing else: no window, no interrupt line, no authority bit. Its whole power
  over the SPI bus is one capability it can call through.
- There is **no interrupt line** (`lines:`) anywhere. This engine polls the controller's FIFOs
  (Stop 8), so it needs none.

Which pins the bus uses is not in the composition at all. It is in the board file, because the
board's wiring decides it:

```yaml
buses:
  spi0:
    device: /dev/dspi0
    pins: [PTD1, PTD2, PTD3]
    chip_selects: [PTC4]
```

`PTD1`, `PTD2` and `PTD3` are the clock, data-out and data-in pins; `PTC4` is the chip select, a
plain GPIO pin the engine drives itself.

The line to notice is `accepts`. The composition tool **admits** a composition before it emits
anything: it checks each grant against what the chip's protection hardware can enforce. On the
K64F the answer for a device is "nothing per thread". The chip file says why:
`covers_devices: false`, because the memory protection unit (the SYSMPU) never sees the peripheral
bus, and the only gate on a device is the peripheral bridge (AIPS), which opens a whole 4 KiB slot
at once and for every unprivileged thread. A grant of `/dev/dspi0` on this chip is therefore a
statement, not an enforcement, and admission refuses it unless the composition says, in writing,
that it accepts both limits.

**Try it:** copy `proxy.yaml` to `/tmp/noaccepts.yaml`, delete the `accepts:` line, and run the
admission alone from `tools/compose`, with the Python the build made for the tool:

```
$ cd tools/compose
$ ../../build/frdmk64f/kickos_compose/venv/bin/python -m kickos_compose admit \
      /tmp/noaccepts.yaml --manifest ../../build/frdmk64f/export/manifest.yaml
/tmp/noaccepts.yaml:13: enforcement.coarse-gate: task `k64dspi` holds `/dev/dspi0`, which the
  0x1000-byte aips_slot of chip `mk64f` opens with what sits beside it, so it needs `coarse_gate`
  in its `accepts`
/tmp/noaccepts.yaml:13: enforcement.device-not-isolated: task `k64dspi` holds `/dev/dspi0`, a
  device window the unit of chip `mk64f` does not see, so it needs `device_not_isolated` in its
  `accepts`
kickos_compose: 2 refusal(s) in 1 composition(s) read
```

The tool exits with code 3, which the configure step turns into a fatal error. The same composition
on a chip whose protection unit covers devices (the XMC4800, at the end of this chapter) needs
neither word.

**Why, and the contract:** [Book: Peripheral isolation and the hardware
ceiling](../book/peripheral-isolation-and-the-hardware-ceiling.md),
[Reference: architecture, Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 2: the build turns the composition and the driver into tables
**Where:** `user/apps/CMakeLists.txt`, `kickos_app_system`; `cmake/compose.cmake` (the
`kickos_compose emit` step); `tools/compose/kickos_compose/emit.py`, `build`, `render`;
`cmake/kickos.cmake`, `kickos_add_driver`, `kickos_select_class_backend`,
`kickos_link_class_backends`; `system/driver/mk64f/k64dspi/CMakeLists.txt`

The application's `CMakeLists.txt` picks the composition (`kickos_app_system(k64dspi COMPOSITION
systems/proxy.yaml)`), and the configure step runs `kickos_compose emit` on it. `build` in
`emit.py` walks the admitted tasks and makes one **task entry** per task and one **grant entry**
per thing a task holds; `render` writes them as C, into
`build/frdmk64f/kickos_compose/k64dspi_system/table.c`. That table is linked into the image as
`kickos_table`, and the init walks it at boot ([01, Stop 16](01-power-on.md)).

Three grants come out of this composition. The driver gets a `KOS_GRANT_WINDOW` (base
`0x4002C000`, size `0x40`) and a `KOS_GRANT_ENDPOINT_SERVE`. The client gets a
`KOS_GRANT_ENDPOINT_USE` with `cap_slot = KOS_SPAWN_DELEGATED_CAP0 + 0`: the index in the client's
handle table where its capability will land. A driver's grants carry no slot and no window
place, because the driver's own **descriptor** says where each of its threads receives what.

That descriptor is the second table. The driver's build line declares its shape:

```cmake
kickos_add_driver(k64dspi SOURCES k64dspi.cc spi_dspi.cc CLASS kickos_class_mk64f REGDIR arch/arm/chip/mk64f
                  WINDOWS regs BLOCK none BARRIER none
                  POSTURE retain START k64dspi_spi_start CLIENT kickos_spi_proxy
                  THREAD service ENTRY bus_thread ARG window WINDOW regs CAPS ep:wait)
```

One thread, `bus_thread`, which receives the window grant, gets the window's base address as its
argument, and holds one capability: the endpoint, with the right to wait on it. `kickos_add_driver`
writes this as `KICKOS_DRIVER_DESCRIPTOR` into a generated header
(`kickos/driver/declared/k64dspi.h`), and `k64dspi.cc` expands it into a `constexpr` descriptor
that two `static_assert`s check at compile time: `drv::valid` (the generic shape rules) and
`spi::desc_ok` (the endpoint sits where `serve_loop` will wait on it).

The third decision the build makes is **which code answers the SPI class**. Every backend of the
class defines the same four public names, so exactly one may be linked into an image.
`system/CMakeLists.txt` chooses with `kickos_select_class_backend`: the proxy by default, the
chip's local engine when `KICKOS_SPI_LOCAL_ENGINE` is on. The application only names the class it
calls (`kickos_link_class_backends(k64dspi spi)`). And the driver task carries its **own private
copy** of the engine: its target renames the four class names (`kos_spi_bus_open=k64dspi_bus_open`
and so on), so the service's engine and the client's proxy can sit in one image without a clash.

**Try it:** read the grants in the emitted table, then the symbols in the image.

```
$ grep -E '\.kind|\.cap_slot|\.base|\.size' build/frdmk64f/kickos_compose/k64dspi_system/table.c
            .kind = KOS_GRANT_WINDOW,
            .cap_slot = KOS_TABLE_NONE,
            .base = 0x4002C000u,
            .size = 0x40u,
            .kind = KOS_GRANT_ENDPOINT_SERVE,
            .cap_slot = KOS_TABLE_NONE,
            ...
            .kind = KOS_GRANT_ENDPOINT_USE,
            .cap_slot = KOS_SPAWN_DELEGATED_CAP0 + 0,
$ arm-none-eabi-nm $PROXY | grep -E ' T (kos_spi|k64dspi)_(bus|device|transfer)'
00002210 T k64dspi_bus_close
00001edc T k64dspi_bus_open
00001f1c T k64dspi_device_open
00002098 T k64dspi_transfer
000019e6 T kos_spi_bus_open
00001a00 T kos_spi_device_open
00001af0 T kos_spi_transfer
```

Two sets of the same functions. The `k64dspi_*` four are the engine, under its private names, used
by the driver thread. The `kos_spi_*` three are the proxy, used by the client (the proxy's
`kos_spi_bus_close` is never called by this client, so the linker dropped it).

**Why, and the contract:** [Reference: bus-service, the bring-up and the client
side](../reference/bus-service.md), [Reference: architecture, Driver packaging: class versus
service](../reference/architecture.md#driver-packaging-class-versus-service)

### Stop 3: the init makes the endpoint, then starts the driver task
**Where:** `system/init/compose/walk.cc`, `Walk::run`, `Walk::boot`, `Walk::scan`, `Walk::start`;
`system/init/compose/driver_path.cc`, `driver_path::start`; `system/driver/mk64f/k64dspi/k64dspi.cc`,
`k64dspi_spi_start`; `user/src/driver_service.cc`, `bring_up`, `instance_bring_up`, `spawn_one`

The init is the first unprivileged program; it holds every authority bit and hands them down
([08, Stop 4](08-spawn-and-caps.md)). `Walk::run` is three steps in a loop: `boot` once, then
`scan` (start what can start) and `wake` (sleep until a task changes state).

`Walk::boot` creates the endpoint before any task exists, one for every task with a
`KOS_GRANT_ENDPOINT_SERVE`. Then it **narrows** its own capability on it to
`KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT`: the init can call, can pass the
capability on, and can hand out the rights it no longer holds, but it can never receive. So the
driver thread will be the endpoint's only receiver. That matters again in Stop 12.

`Walk::scan` goes through the tasks in file order. The driver uses nothing, so it starts first.
Because its table entry names a driver, `Walk::start` does not create the task itself: it calls
`driver_path::start`, which fills a `kos_driver_instance` (the window's base and size from the
grant, the priority, the endpoint, a badged copy of the init's notification to watch the task
with) and calls the start function the table points at, `k64dspi_spi_start`. That function runs
**on the init's thread**:

```cpp
    int k64dspi_spi_start(struct kos_driver_instance* instance)
    {
        // Here, on the init's thread: the driver's threads hold no pinmux authority.
        int32_t const muxed = k64dspi_bus_mux(instance->mmio_base);
        if (muxed != 0)
        {
            kos::print("[k64dspi] ERROR: the board bus over the DSPI window was not muxed\n");
            return muxed;
        }
        return drv::bring_up(k_desc, instance);
    }
```

Pin muxing chooses what a pin is for (GPIO, SPI clock, UART, ...). It goes through the
`kos_pinmux_set` syscall, which the kernel allows only to a holder of the `pinmux` authority bit.
The init holds it; the driver thread will not. So the one privileged step of the bring-up happens
here, before the driver exists. `k64dspi_bus_mux` (renamed `k64dspi_service_bus_mux` in this
target) looks the bus up by its base in the generated `board_buses.h`, sets the three bus pins to
their SPI function (alternative 2) and the chip select to GPIO.

Then `bring_up` follows the descriptor. It checks it (`valid`), checks the instance against it
(`instance_admits`), and in `instance_bring_up` creates the task (`kos_task_create`), grants it its
priority ceiling (`kos_task_sched_grant`), sets the init's watch on it (`kos_task_watch`, with the
endpoint as the readiness signal), and spawns the one thread with `spawn_one`. `spawn_one` turns
the descriptor's thread into a `kos_thread_create` call: the window `{0x4002C000, 0x40}` as a
device window, one delegated capability (the endpoint, taken from the init's capability with the
rights mask `KOS_CAP_WAIT`: the init could hand out a right it does not hold because it kept
`KOS_CAP_HANDOUT`), the window base as the thread's argument, and **authority 0**. The spawn
itself is the path [08](08-spawn-and-caps.md) walks from Stop 8 on.

**Try it:** the start shim's calls, in the image.

```
$ arm-none-eabi-objdump -d -C --no-show-raw-insn $PROXY --disassemble=k64dspi_spi_start | grep -E 'bl|b\.w'
    1e2e:	bl	1e98 <k64dspi_service_bus_mux>
    1e3e:	b.w	2c3c <kickos::driver::bring_up(kickos::driver::Descriptor const&, kos_driver_instance*)>
    1e44:	bl	25fe <kos_print>
```

On a board: `break k64dspi_spi_start`, `continue`, then `p *instance`. Expect `mmio_base =
0x4002c000`, `mmio_window = 64`, `priority = 12`, and `task = 0` (no task yet; `KOS_TASK_NONE`).
<!-- unverified: needs a FRDM-K64F -->

**Why, and the contract:** [Reference: bus-service, The bring-up](../reference/bus-service.md),
[01, Stop 16](01-power-on.md) (the init reading its table), [08, Stop 4](08-spawn-and-caps.md)
(the init handing authority down)

### Stop 4: the driver thread brings DSPI0 up
**Where:** `system/driver/mk64f/k64dspi/k64dspi.cc`, `bus_thread`;
`system/driver/mk64f/k64dspi/spi_dspi.cc`, `kos_spi_bus_open` (linked as `k64dspi_bus_open`),
`chip_selects_idle`; `kernel/syscall/syscall.cc`, the `KOS_SYS_PERIPH_ENABLE` arm;
`kernel/syscall/syscall_mem.cc`, `caller_holds_mmio_block`; `arch/arm/chip/mk64f/chip_mk64f.cc`,
`arch_periph_enable`

The driver thread starts unprivileged, at priority 12, holding one window and one capability.
Its first act is the bus bring-up, through the class:

```cpp
    void bus_thread(void* arg)
    {
        struct kos_spi_bus_config cfg;
        cfg.base = reinterpret_cast<uintptr_t>(arg);
        cfg.ep = KOS_CAP_NONE;  // a local engine reaches no endpoint
        cfg.irq = KOS_CAP_NONE; // the DSPI pump polls its FIFOs
        // ...
        struct kos_spi_bus bus;
        int32_t const opened = kos_spi_bus_open(&bus, &cfg);
        if (opened < 0)
        {
            kos::print("[k64dspi] ERROR: bus bring-up refused, DSPI0 unreachable\n");
            drv::trap();
        }
```

The thread is a *local* user of the class: it holds the window, so it fills `base` and leaves
`ep` empty. The `kos_spi_bus_open` it calls is the engine's, under its private name. The engine
does three things, in this order:

1. **Chip selects to idle** (`chip_selects_idle`). For each chip select the board wires to this
   bus, it writes the pin's GPIO `PSOR` (set) register, then makes the pin an output in `PDDR`.
   High first, output second, so the pin never drives low for an instant. These are direct
   stores from an unprivileged thread with no grant at all: on the K64F the GPIO block sits
   behind no bridge slot, so every thread reaches it (the chip file says so beside the `gpio`
   device).
2. **Clock and open the controller** (`kos_periph_enable`). Until this returns, DSPI0 is
   unclocked and its bridge slot is supervisor-only: an unprivileged load from `0x4002C000`
   would bus-fault. The syscall's check is **possession**: `caller_holds_mmio_block` asks
   whether the calling thread holds a window whose base is exactly this one. No authority bit is
   involved. If it does, `arch_periph_enable` sets `reg::sim::SCGC6_SPI0` in the SIM's `SCGC6`
   clock-gate register, then clears the supervisor-protect bit of DSPI0's bridge slot
   (`reg::aips::pacr_of`, `reg::aips::pacr_sp_bit`, slot 44), clock first, so the
   slot is never open on an unclocked block.
3. **Enable the controller** (two `MCR` stores). It resets disabled and halted; the first store
   makes it a master and flushes both FIFOs, the second releases the halt.

On this chip the window grant has no effect on the bus itself (the SYSMPU never sees it), but it
is the only thing that lets this thread call `kos_periph_enable`. That is why the build
declaration keeps it; the comment above the `kickos_add_driver` line says so. And once the slot
is open, it is open for every unprivileged thread on the chip, which is exactly what
`coarse_gate` and `device_not_isolated` accepted in Stop 1.

**Try it:** the bring-up's calls, in the image.

```
$ arm-none-eabi-objdump -d -C --no-show-raw-insn $PROXY --disassemble=k64dspi_bus_open | grep -E 'bl|str'
    1ee8:	str	r3, [r4, #0]
    1eec:	str	r3, [r4, #4]
    1ef0:	str	r3, [r4, #8]
    1ef2:	bl	1e54 <(anonymous namespace)::chip_selects_idle(unsigned int)>
    1ef6:	bl	25ac <kos_periph_enable>
    1f00:	str	r3, [r4, #0]
    1f02:	str	r2, [r3, #0]
    1f08:	str	r2, [r3, #0]
$ arm-none-eabi-objdump -d --no-show-raw-insn $PROXY --disassemble=kos_periph_enable | grep -E 'movs|bl'
    25ae:	movs	r3, #0
    25b6:	movs	r0, #39	@ 0x27
    25b8:	bl	44b6 <arch_syscall>
```

The first three stores into `r4` fill the `kos_spi_bus` struct (its base stays 0 until the clock
is on); the fourth, after the call returns, records the base; the two stores through `r3` at the
end are the two `MCR` writes; `39` is `KOS_SYS_PERIPH_ENABLE`.

On a board: `break '(anonymous namespace)::bus_thread'`, `continue`, `p/x $r0`. Expect
`0x4002c000`: the window base, passed as a value. Then `break arch_periph_enable`, `continue`,
`p/x base` (expect `0x4002c000`), `finish`, and `x/wx 0x40000044` to read the slot's `PACR`
register after the clear (bit 14 should be 0). <!-- unverified: needs a FRDM-K64F -->

**Why, and the contract:** [Book: Authority from possession or from
identity](../book/authority-from-possession-or-from-identity.md), [Reference: bus-service, K64F
DSPI0 register map](../reference/bus-service.md)

### Stop 5: the driver parks in its receive, and the init starts the client
**Where:** `user/include/kickos/sys/spi_service.h`, `serve_loop`; `kernel/task/task.cc`,
`task_note_receive`; `system/init/compose/walk.cc`, `Walk::wake`, `Walk::startable`,
`Walk::serving`, `Walk::spawn`

After printing `[k64dspi] SPI service up (DSPI0, polled FIFO, GPIO CS)`, the driver thread
enters `serve_loop` and never leaves it while the system runs:

```cpp
    kos_reply_recv_opts_init(&opts, KOS_SPI_CAP_EP, 0u, KOS_TIMEOUT_NONE);
    // Send this reply when receiving the next request.
    kos_cap_t reply_cap = KOS_CAP_NONE;
    size_t reply_len = 0;
    while (true)
    {
        opts.info.reply_cap = KOS_CAP_NONE;
        long const n = kos_reply_recv(reply_cap, msg,
                                      kos_call_lens_pack(reply_len, sizeof(msg)), &opts);
        // ...
        reply_len = serve_one(bus, slots, msg, static_cast<size_t>(n));
        reply_cap = opts.info.reply_cap;
    }
```

`kos_reply_recv` is the fused "reply to the last caller, then wait for the next one" syscall. On
the first pass there is no caller yet (`reply_cap` is `KOS_CAP_NONE`), so it only waits, on the
endpoint at `KOS_SPI_CAP_EP`, which is `KOS_SPAWN_DELEGATED_CAP0`: the slot `spawn_one` put it
in. The thread parks ([05, Stop 3](05-call-and-reply.md)). The device handles live in `SlotTable
slots` on this thread's own stack, so the driver keeps no per-device state in `.bss`.

Parking is also the driver's signal that it is ready. The init's `kos_task_watch` named the
endpoint, so the first time a member of the task waits on it, `task_note_receive` marks the task
ready and raises the init's watch bit. The init, asleep in `Walk::wake`, reads the task's state,
sees `KOS_TASK_READY`, and records it.

On the next `Walk::scan`, `Walk::startable` lets the client start: every task it `uses` is now
`serving`. This is what the composition's `uses:` line buys. A client is never started against an
endpoint nobody receives on, so its first call cannot meet an empty endpoint. The client is a
plain task, so `Walk::start` creates it itself, and `Walk::spawn` turns its one grant into a
delegated capability: source the init's endpoint capability, rights mask `KOS_CAP_SIGNAL`, landing
at slot `KOS_SPAWN_DELEGATED_CAP0 + 0`. The client cannot receive on the endpoint, and since its
copy carries no `KOS_CAP_TRANSFER`, it cannot pass it on either.

**Try it:** in the image, `serve_loop` is inlined into `bus_thread`: the disassembly in Stop 12
shows `bus_thread` calling `kos_reply_recv` and `serve_one` directly. On a board,
`break kickos::task_note_receive`, `continue`: the first stop is the driver thread's first
receive (nothing else in this system receives on a watched endpoint). Then `break k64dspi_main`,
`continue`, and `p *self` to see the client's table entry (`priority = 9`, `cap_grant_count = 1`).
<!-- unverified: needs a FRDM-K64F -->

**Why, and the contract:** [Reference: Synchronous call/reply IPC](../reference/ipc-call-reply.md)
(`KOS_SYS_REPLY_RECV`), [08, Stop 4](08-spawn-and-caps.md), [09, Stop 9](09-fault-and-kill.md)
(the watch)

### Stop 6: the client opens the bus through the SPI class
**Where:** `user/apps/frdmk64f/k64dspi/main.cc`, `k64dspi_main`, `open_device`;
`user/src/grant_lookup.cc`, `kos_grant_endpoint`; `user/lib/spi_proxy/spi_proxy.cc`,
`kos_spi_bus_open`, `kos_spi_device_open`, `seat_req`

The client's entry receives `self`, its own entry in the init's table. It looks its capability up
by the name the composition used:

```cpp
    kos_cap_t const ep = kos_grant_endpoint(self, "/svc/spi0");
    if (ep == KOS_CAP_NONE)
    {
        kos::print("[k64dspi] ERROR: no /svc/spi0 endpoint\n");
        exit(1);
    }
    bcfg.base = 0u;
    bcfg.ep = ep;
```

`kos_grant_endpoint` walks this task's grants in the table, finds the one named `/svc/spi0`, and
returns its `cap_slot`. No kernel call; the slot was decided at build time. The client fills the
field it holds, `ep`, and leaves `base` empty: the opposite of the driver in Stop 4. The struct is
the same; there is no flag saying which kind of user filled it.

Then `open_device` makes two class calls. Both go to the proxy, because that is what the build
linked as the class.

- **`kos_spi_bus_open`** sends nothing. It checks there is an endpoint and records it. Bringing
  the controller up belongs to whoever holds its window, and that was done in Stop 4.
- **`kos_spi_device_open`** asks for a device profile: SPI mode 0, 8-bit words, 10 MHz, chip select
  by GPIO, slot 0. The proxy frames it as a `KOS_BUS_OP_CONFIG` request: a 12-byte `kos_bus_req`
  header (`seat_req` writes `proto = KOS_BUS_SPI`, `op`, `device = 0`, `region_cap = -1`, meaning
  the payload is inline in the message) followed by a 12-byte `kos_bus_cfg`. It calls with
  `kos_call(b->ep, buf, 24, 24, KOS_TIMEOUT_NONE)`. The service opens the device on its own engine (Stop 8 shows how
  the service dispatches) and replies the bit clock the controller will really run at; the proxy
  stores that in `d->hz` and returns it. The class contract forbids echoing the request back: if
  the far side reports 0 Hz, the proxy returns `-KOS_ENOTSUP`.

The profile itself (the folded `CTAR0` word, the chip-select pin) lives only in the service's
`SlotTable`. The client's `kos_spi_device` keeps `prog[0] = prog[1] = 0`: it is a handle naming a
slot, nothing more.

**Try it:** what the client's entry calls, in each image.

```
$ arm-none-eabi-objdump -d -C --no-show-raw-insn $PROXY --disassemble=k64dspi_main | grep -E '\bbl\b' | head -4
     426:	bl	3028 <kos_grant_endpoint>
     430:	bl	25fe <kos_print>
     440:	bl	19e6 <kos_spi_bus_open>
     45a:	bl	1a00 <kos_spi_device_open>
```

On a board: `break kos_spi_device_open`, `continue`, `finish`. Expect the return value in `$r0` to
be the achieved rate. With the core at 120 MHz the DSPI runs off a 60 MHz bus clock, and a 10 MHz
request divides exactly (`/3` then `/2`), so expect `10000000`, and the client's line
`[k64dspi] device open rc=0 achieved=10000000 Hz`. <!-- unverified: needs a FRDM-K64F -->

**Why, and the contract:** [Reference: bus-service, the KOS_BUS_OP_CONFIG op and the client
side](../reference/bus-service.md)

### Stop 7: the proxy frames a transfer
**Where:** `user/apps/frdmk64f/k64dspi/main.cc`, `read_byte_test`; `user/lib/spi_proxy/spi_proxy.cc`,
`kos_spi_transfer`; `user/include/kickos/driver/spi.h`, `kos_spi_seg_check`, `KOS_SPI_XFER_MAX`

The client's test reads the LAN9252's `BYTE_TEST` register: a command byte `0x03`, a 16-bit
address `0x0064`, then four bytes clocked in, all under one chip-select assertion. It describes
that as one buffer of 7 bytes and two **segments** (3 bytes, then 4). SPI is full duplex: every
clock edge sends one bit and receives one, so the buffer holds the bytes to send on entry and the
bytes received on return. The register's value is in the last four bytes.

The proxy's transfer:

```cpp
    unsigned char msg[KOS_EP_MSG_MAX];
    size_t const framing =
        sizeof(struct kos_bus_req) + static_cast<size_t>(nseg) * sizeof(struct kos_bus_seg);
    seat_req(msg, KOS_BUS_OP_XFER, d->slot, nseg);
    mem_copy(msg + sizeof(struct kos_bus_req), seg,
             static_cast<size_t>(nseg) * sizeof(struct kos_bus_seg));
    mem_copy(msg + framing, buf, len);

    int32_t const rc = kos_call(b->ep, msg, framing + len, sizeof(msg), KOS_TIMEOUT_NONE);
```

The message is `kos_bus_req` (12 bytes), two `kos_bus_seg` (4 bytes each), then the 7 payload
bytes: 27 bytes in all. The reply capacity is the whole 256-byte buffer, `KOS_EP_MSG_MAX`.

Before any of that, `kos_spi_seg_check` runs **here, in the client**. It refuses a transfer
whose segment lengths do not add up to `len`, a segment with flags set (those are I2C's), and any
`len` above `KOS_SPI_XFER_MAX`. That ceiling is 212 bytes: 256 minus the worst-case framing of 44
bytes. It is checked on the client side because a transfer larger than a message has no message to
be refused in. Every backend applies the same check, so a client written against the class gets
the same refusal whichever backend it is linked with.

**Try it:** the proxy's transfer, in the image.

```
$ arm-none-eabi-objdump -d -C --no-show-raw-insn $PROXY --disassemble=kos_spi_transfer | grep -E '\bbl\b'
    1b7c:	bl	19d4 <mem_copy(void*, void const*, unsigned int)>
    1b86:	bl	19d4 <mem_copy(void*, void const*, unsigned int)>
    1b92:	bl	19d4 <mem_copy(void*, void const*, unsigned int)>
    1ba4:	bl	22da <kos_call>
    1bb6:	bl	19d4 <mem_copy(void*, void const*, unsigned int)>
    1bdc:	bl	19d4 <mem_copy(void*, void const*, unsigned int)>
```

Three copies in (header, segments, payload), the call, then the copies of the reply header and the
received bytes back out. The proxy has no register access at all.

On a board: `break kos_call`, `continue` twice (the first stop is the CONFIG of Stop 6), then
`p $r2` and `p $r3`. Expect `27` and `256`. <!-- unverified: needs a FRDM-K64F -->

**Why, and the contract:** [Reference: bus-service, Framing and the inline budget, The segment
model](../reference/bus-service.md)

### Stop 8: the call carries it, and the driver runs the transaction
**Where:** `user/src/syscall_stubs.cc`, `kos_call`, `kos_call_generic`; `kernel/syscall/syscall_ipc.cc`,
`endpoint_call`, `endpoint_reply_recv`; `user/include/kickos/sys/spi_service.h`, `serve_one`;
`system/driver/mk64f/k64dspi/spi_dspi.cc`, `kos_spi_transfer` (linked as `k64dspi_transfer`),
`cs_write`; `arch/arm/chip/mk64f/class/dspi_class.cc`, `dspi_rx_count`

`kos_call` has two roads. The Cortex-M build has a register fastpath for small calls: if both the
message and the reply capacity fit in `KOS_CALL_REG_BYTES` (20), the payload travels in registers
through `KOS_SYS_CALL_REG` ([05, Stop 11](05-call-and-reply.md)). A bus request never fits: the
CONFIG is 24 bytes, this transfer 27, and the transfer's reply capacity is 256. So every bus call
takes `kos_call_generic`, the `KOS_SYS_CALL` syscall, and the path [05](05-call-and-reply.md)
walks from Stop 4 to Stop 8: the kernel finds the driver parked in its receive, copies the 27
bytes straight into the driver's `msg`, mints a one-shot **reply capability** into the driver's
handle table, parks the client and switches to the driver.

The client runs at priority 9 and the driver at 12, so the driver already outranks its caller.
The priority the call lends ([05, Stop 6](05-call-and-reply.md)) changes nothing here; it matters
when a higher-priority client calls a lower-priority service.

The driver wakes in `kos_reply_recv` with `n = 27` and the reply capability in
`opts.info.reply_cap`, and calls `serve_one`. For an `XFER` request it checks the header
(`proto`, `region_cap == -1`, `offset == 0`, the slot in range and opened by a CONFIG, `nseg`
between 1 and 8), reads the segments, checks the lengths fit the message and the class ceiling,
then moves the payload down to just after where the 4-byte reply header will go, and runs the
transfer on the slot's handle:

```cpp
    unsigned char* work = buf + sizeof(struct kos_bus_rsp);
    mem_copy(work, buf + framing, total);

    int32_t const moved = kos_spi_transfer(&slots.dev[req.device], seg,
                                           static_cast<uint8_t>(req.nseg), work, total);
```

That `kos_spi_transfer` is the engine's. It halts the controller, writes the slot's profile into
`CTAR0` (each transfer re-applies its own device's profile, because the controller has one live
profile register for all devices), and releases the halt. Then it asserts the chip select with one
store to the pin's GPIO `PCOR` (clear) register, pumps the bytes, and releases the chip select with
one store to `PSOR`:

```cpp
    cs_write(d, GPIO_PCOR);
    // ...
    while (popped < len)
    {
        bool moved = false;
        if (kickos::mk64f::driver::dspi_rx_count(win) > 0u)
        {
            buf[popped] = static_cast<unsigned char>(*popr & 0xFFu);
            popped++;
            moved = true;
        }
        if (pushed < len and (pushed - popped) < RX_FIFO_DEPTH
            and ((*sr >> 12) & 0xFu) < TX_FIFO_DEPTH)
        {
            *pushr = static_cast<uint32_t>(buf[pushed]) & 0xFFu;
            pushed++;
            moved = true;
        }
        // ... a poll that moved nothing counts toward stall_polls; at the bound the engine
        // halts the controller, releases the chip select and returns -KOS_EIO
    }

    cs_write(d, GPIO_PSOR);
```

This is a **polled** transaction: the driver thread spins on the FIFO counters, it does not
sleep on an interrupt. The spin is bounded: a frame that never completes (the module halted, a
received byte lost) ends the transfer once a count of polls has moved no byte, and the engine then
halts the controller, releases the chip select and returns `-KOS_EIO`. The controller's FIFOs are 4 deep each way. Each turn of the loop first
drains one received byte if there is one (at `POPR_OFFSET`), then pushes the next byte to send
(at `PUSHR_OFFSET`) if fewer than four are in flight and the TX FIFO has room. The in-flight
bound is what keeps the RX FIFO from overflowing: a byte that completes always finds a free slot.
Seven bytes at 10 MHz are 56 bit times, 5.6 microseconds of clocking, and the driver, at priority
12, holds the CPU for all of it. Nothing below priority 12 runs during a transfer.

Notice what is *not* here. The chip select is two plain stores, not two syscalls. A syscall per
edge would cost more than the 7-byte transfer itself; the GPIO block is reachable without a grant
on this chip, and the bus pins were muxed once, at start. This is the case the Book chapter on the
fast path argues: configure once through the checked path, toggle directly. On a chip that drives
the chip select from the controller's own pin (the XMC4800's `SELO0`), there is no GPIO store at
all.

[07](07-interrupt-to-a-driver.md) walks the other way to pace a driver: an interrupt line bound to
a notification, the driver sleeping in `notify_wait`, the kernel masking the line, the driver
acknowledging it. The K64F engine needs none of that, which is why the composition gives it no
line. The XMC4800 service at the end of this chapter takes that interrupt-paced road.

**Try it:** the service's copy of the engine, in the image.

```
$ arm-none-eabi-objdump -d -C --no-show-raw-insn $PROXY --disassemble=k64dspi_transfer | grep -E '#12\]|#44|#52|#56|0xc000|rx_count|cs_write'
    20fe:	str.w	r3, [r8, #12]
    2108:	str.w	r3, [r9], #44
    2110:	bl	1e78 <(anonymous namespace)::cs_write(kos_spi_device const*, unsigned int)>
    2190:	bl	32f8 <kickos::mk64f::driver::dspi_rx_count(unsigned int)>
    2198:	ldr.w	r2, [r8, #56]	@ 0x38
    21b4:	ands.w	r2, r2, #49152	@ 0xc000
    21be:	str.w	r1, [r8, #52]	@ 0x34
    21d2:	bl	1e78 <(anonymous namespace)::cs_write(kos_spi_device const*, unsigned int)>
    21f0:	bl	1e78 <(anonymous namespace)::cs_write(kos_spi_device const*, unsigned int)>
$ arm-none-eabi-objdump -s -j .text --start-address=0x2200 --stop-address=0x2204 $PROXY | tail -1
 2200 010c0080                             ....
```

`r8` is the window base. `#12` is `CTAR0_OFFSET`, `#56` (`0x38`) is `POPR_OFFSET`, `#52`
(`0x34`) is `PUSHR_OFFSET`; the `MCR` store that releases the halt post-increments `r9` by 44 to
`SR_OFFSET` (`0x2C`). The compiler turned `((*sr >> 12) & 0xF) < 4` into a test of bits 14 and 15.
The chip select is `cs_write`, a small function of its own: the first call passes `r1 = 8`
(`PCOR`), the last passes `r1 = 4` (`PSOR`), and the call at `21f0` is the same release on the
stall path. The first word of the literal pool, at `2200`, is `0x80000C01` (`MCR_MSTR |
MCR_CLR_TXF | MCR_CLR_RXF | MCR_HALT`), the store that halts the controller at `20f8` before
CTAR0 is written and again on the stall path.

On a board: `break k64dspi_transfer`, run until the client's BYTE_TEST call, then
`set $b = $r3` and `x/7xb $b` (the outgoing buffer: expect `03 00 64 00 00 00 00`), `finish`,
and `x/7xb $b` again: the last four bytes should read `21 43 65 87`, the LAN9252's `0x87654321`
least-significant byte first. <!-- unverified: needs a FRDM-K64F with the EasyCAT shield -->

**Why, and the contract:** [Book: The fast path is the
capability](../book/the-fast-path-is-the-capability-gpio-direct-mmio.md),
[Reference: bus-service, Chip-select policy and Device slots](../reference/bus-service.md),
[05](05-call-and-reply.md) (the rendezvous), [07](07-interrupt-to-a-driver.md) (the
interrupt-paced alternative)

### Stop 9: the reply, and its two status channels
**Where:** `user/include/kickos/sys/spi_service.h`, `serve_one` (its tail), `reply_error`,
`serve_loop`; `kernel/syscall/syscall_ipc.cc`, `endpoint_reply_recv`;
`user/lib/spi_proxy/spi_proxy.cc`, `reply_payload`

`serve_one` writes a `kos_bus_rsp` over the start of the same buffer: `status = 0`, `len = 7`,
followed by the 7 received bytes already in place. It returns 11, the reply's length. The reply is
not sent from `serve_one`: `serve_loop` keeps the reply capability and the length, and sends them
as the first half of its next `kos_reply_recv`. One syscall answers this client and waits for the
next one. The kernel copies the 11 bytes into the client's `msg` (a call is in place: the reply
lands in the buffer the request left from), consumes the reply capability, wakes the client and
parks the driver ([05, Stop 8](05-call-and-reply.md)).

Every path through `serve_one` returns a length, the error paths included (`reply_error` builds a
header with a negative status and no payload). That is the service's half of the call contract:
a reply capability the loop forgot to answer would leave its client parked forever, since
`kos_call` has no deadline.

The client now holds two answers, and they mean different things:

```cpp
    int32_t reply_payload(unsigned char const* buf, int32_t rc, size_t* off)
    {
        if (rc < 0)
        {
            return rc;
        }
        if (static_cast<size_t>(rc) < sizeof(struct kos_bus_rsp))
        {
            return -KOS_EINVAL; // reply too short to carry a status header
        }
        struct kos_bus_rsp rsp;
        mem_copy(&rsp, buf, sizeof(rsp));
        if (rsp.status < 0)
        {
            return rsp.status;
        }
```

- **`kos_call`'s return** is the kernel's answer: did the call reach a receiver and come back.
  Here it is 11, the reply's length. A negative value means the transport failed (Stop 12 shows
  `-KOS_EPIPE`, `-KOS_EAGAIN`, `-KOS_ECONNREFUSED`), and then there is no reply to read.
- **`kos_bus_rsp.status`** is the service's answer: did the transaction succeed. It means
  something only when the call itself succeeded.

A concrete case where they differ: open a device with `cs_policy = KOS_BUS_CS_HW`. The DSPI engine
refuses hardware chip select (releasing it clocks a trailing byte). The call goes through fine,
`kos_call` returns 4, and `status` is `-KOS_ENOTSUP`. The proxy folds both into one negative return
for the class caller, so the client sees the same codes a local engine would return.

`kos_spi_transfer` in the proxy then checks the reply carries exactly `len` bytes (a full-duplex
transfer that clocked returns as many bytes as it sent), copies them into the caller's `buf`, and
returns 7.

**Try it:** `reply_payload` is inlined into its callers (`nm` does not list it), so stop on the
call instead. On a board: `break kos_call`, `continue` to the transfer (the second stop),
`set $m = $r1` to remember the buffer, `finish`, then `p $r0` (expect `11`) and `x/4xb $m`
(expect `00 00 07 00`: status 0, then length 7, little-endian).
<!-- unverified: needs a FRDM-K64F -->

**Why, and the contract:** [Reference: bus-service, Layering and Service-side
discipline](../reference/bus-service.md), [Reference: ipc-call-reply](../reference/ipc-call-reply.md)
(`KOS_SYS_REPLY_RECV`), [Book: Synchronous call/reply](../book/synchronous-call-and-reply.md)

### Stop 10: the same client, built to own the controller itself
**Where:** `user/apps/frdmk64f/k64dspi/systems/local.yaml`; `user/apps/frdmk64f/k64dspi/CMakeLists.txt`;
`user/apps/frdmk64f/k64dspi/main.cc`, `k64dspi_main` (the `KICKOS_SPI_LOCAL` branch);
`system/CMakeLists.txt` (`KICKOS_SPI_LOCAL_ENGINE`); `system/driver/mk64f/k64dspi/spi_dspi.cc`,
`k64dspi_bus_mux`

With `-DKICKOS_SPI_LOCAL_ENGINE=ON`, two things change at once. `system/CMakeLists.txt` selects
`kickos_spi_k64dspi`, the engine under its public names, as the SPI class for the image. And the
application's `CMakeLists.txt` picks the other composition:

```yaml
tasks:
  - name: client
    entry: k64dspi_main
    priority: 9
    ceiling: 9
    devices: [/dev/dspi0]
    accepts: [coarse_gate, device_not_isolated]
    authority: [pinmux]
```

One task. No driver, no endpoint. The client holds the window itself, takes the same two
`accepts`, and gets the `pinmux` authority bit, because nobody else will mux the pins for it. Its
entry takes the other branch of the same source file:

```cpp
#if KICKOS_SPI_LOCAL
    kos_window_t const window = kos_grant_mmio(self, "/dev/dspi0");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    // ...
    int32_t const muxed = k64dspi_bus_mux(win);
    // ...
    bcfg.base = win;
    bcfg.ep = KOS_CAP_NONE;
#else
```

From there, `run_client` is the same code, calling the same four class names. They now land in
the engine: `kos_spi_bus_open` runs Stop 4 in the client's own thread, `kos_spi_device_open`
folds the profile into the client's own handle, `kos_spi_transfer` is the polled loop of Stop 8.
No frame, no `kos_call`, no context switch, no copy of the payload through the kernel.

What moved, and what that says about the trade:

- **Cost.** The service road costs, per transfer, the framing copies, one call and one reply
  (two context switches) and the kernel's copy of the message both ways. The local road costs the
  register work and nothing else. For a client in a tight cyclic loop, a fieldbus stack for
  instance, that difference is the point of the local build.
- **Authority.** In the service build the client's whole power is one `SIGNAL` capability on one
  endpoint. In the local build it holds the window (and so may call `kos_periph_enable`), and it
  holds `pinmux`, which the kernel checks only as a bit: `kos_pinmux_set` will re-mux any pin on
  the chip the kernel does not own. That is a much larger grant than "talk to the SPI bus".
- **Faults.** In the service build the transaction runs on the driver's stack, between the
  driver's own two chip-select stores. In the local build it runs in the client, with the
  client's buffer (Stop 11).
- **What stays the same on this chip.** The register isolation. On the K64F the opened DSPI0 slot
  is reachable by every unprivileged thread in both builds; both compositions carry the same
  `accepts`. On a chip whose protection unit covers devices, the service build would also keep
  the client out of the controller's registers; here it cannot.

The client did not change. Which backend answers the class is the image's decision, made by one
option, and the composition says who holds which capability. That is the
[architecture](../reference/architecture.md#driver-packaging-class-versus-service) rule "the
consumer chooses the coupling" seen from the code.

**Try it:** compare the two images and the two tables.

```
$ arm-none-eabi-nm $LOCAL | grep -E ' T (kos_spi|k64dspi)_'
000018c4 T k64dspi_bus_mux
00000410 T k64dspi_main
00001908 T kos_spi_bus_open
00001948 T kos_spi_device_open
00001ac4 T kos_spi_transfer
$ arm-none-eabi-objdump -d -C --no-show-raw-insn $LOCAL --disassemble=k64dspi_main | grep -E '\bbl\b' | head -4
     42a:	bl	20e0 <kos_grant_mmio>
     43c:	bl	18c4 <k64dspi_bus_mux>
     44e:	bl	1908 <kos_spi_bus_open>
     468:	bl	1948 <kos_spi_device_open>
$ arm-none-eabi-size $PROXY $LOCAL
   text	   data	    bss	    dec	    hex	filename
  68312	   1544	 149024	 218880	  35700	.../frdmk64f/user/apps/frdmk64f/k64dspi/k64dspi
  63132	   1544	 149024	 213700	  342c4	.../frdmk64f-local/user/apps/frdmk64f/k64dspi/k64dspi
```

No `k64dspi_transfer`, no `bus_thread`, no `serve_one`: the public `kos_spi_transfer` is now the
engine itself (it starts with the segment check, then stores `0x80000C01` to `MCR`). In the
emitted table (`build/frdmk64f-local/kickos_compose/k64dspi_system/table.c`), the one task has
`.authority = KOS_AUTH_PINMUX`, and its window grant has `.window = 0`, a place in the client's
own spawn list, where the driver's grant had `KOS_TABLE_NONE`.

**Why, and the contract:** [Reference: bus-service, The client side is the
CLASS](../reference/bus-service.md), [Book: The fast path is the
capability](../book/the-fast-path-is-the-capability-gpio-direct-mmio.md)

### Stop 11: when the client fails
**Where:** `user/include/kickos/sys/spi_service.h`, `serve_loop`; `user/include/kickos/sys/serve.h`,
`serve_transaction_failed`; `kernel/thread/park.cc`, `thread_abort_park`; `kernel/syscall/cap.cc`,
`cap_reply_thread`, `endpoint_rights_dropped`; `system/init/compose/walk.cc`, `Walk::wake`, `Walk::end`

Ask what a broken client can do to the driver, in the service build. Read it from the driver's
side: everything the client can reach is its `SIGNAL` capability, so everything it can do is
call, or stop calling.

- **A bad buffer.** The proxy builds its message in its own stack buffer, so a bad `buf` faults
  the client in its own `mem_copy`, before any call. A bad buffer passed to `kos_call` directly is
  refused by the kernel at the call with `-KOS_EFAULT`, before the driver sees anything. If the
  reply cannot be copied back into the client's buffer when the driver answers, the reply half of
  the driver's next `kos_reply_recv` answers `-KOS_EFAULT`. That code names the client, not the
  server, and `serve_loop` treats it so: `serve_transaction_failed` returns true for
  `-KOS_EFAULT` alone, and the loop receives again. One client cannot end the service.
- **A malformed request.** Every check in `serve_one` answers with `reply_error`. The driver never
  trusts a length it was sent: it checks the framing fits the message and the total fits the
  class ceiling before it copies.
- **The client dies mid-call.** Its parked thread is unwound by `thread_abort_park`, which leaves
  the driver holding the reply capability (reaching into the driver's table to take it back would
  cross the boundary between the two tasks) and bumps the dead call's sequence number. The
  driver's next `kos_reply_recv` resolves the capability in `cap_reply_thread`, finds no caller
  parked on that call, answers the reply half `-KOS_ESRCH`, and, for that code, still runs the
  receive half. The service just takes the next request.
- **The client's capability closes.** It is a `SIGNAL` capability, so closing it does not touch
  the endpoint's receiver count. `endpoint_rights_dropped` acts only when a `WAIT`-bearing
  capability goes.

In this composition one more thing happens: `ends: client`. The client is the task whose end ends
the system. When it exits or faults, `Walk::wake` sees `KOS_TASK_ENDED` on it and calls
`Walk::end`, which shuts the system down with the client's exit status. The driver is stopped with
everything else, but not because of anything the client did to it.

In the local build, the same fault means something else. The transaction runs in the client. If
the client faults inside `kos_spi_transfer`, say on a bad `buf` in the pump loop, it dies between
the `PCOR` store and the `PSOR` store: nothing releases the chip select, and the device on the bus
stays selected. In the service build that cannot happen, because the bytes the engine touches are
in the driver's own `msg`, copied there by the kernel, and the transaction finishes before the
reply is sent.

**Try it:** on a board, in a scratch copy of the client, call the endpoint directly with a buffer
the client was not granted, such as the kernel's `kickos::detail::g_instance` (`nm -C $PROXY`
gives `1fff27d8 B kickos::detail::g_instance` on this build):
`kos_call(ep, (void*)0x1fff27d8, 27, 256, KOS_TIMEOUT_NONE)`. Expect `-14` (`-KOS_EFAULT`), and the driver to stay
parked: `break k64dspi_transfer` never fires for that call. <!-- unverified: needs a FRDM-K64F -->

**Why, and the contract:** [Reference: ipc-call-reply, KOS_SYS_REPLY_RECV (the reply half's failures
are not symmetric)](../reference/ipc-call-reply.md), [09](09-fault-and-kill.md)

### Stop 12: when the driver dies
**Where:** `system/driver/mk64f/k64dspi/k64dspi.cc`, `bus_thread`; `user/src/driver_service.cc`,
`trap`; `kernel/syscall/cap.cc`, `obj_close_protocol` (the `CAP_REPLY` arm), `endpoint_rights_dropped`,
`refuse_senders`; `kernel/include/kickos/endpoint.h`, `endpoint_unserved`;
`system/init/compose/walk.cc`, `Walk::wake`, `Walk::death`, `Walk::drop`, `Walk::propagate`

A driver thread that cannot go on does not return or `exit`: it calls `drv::trap`, which executes
an illegal instruction in a loop. Its task then dies by the fault path, the same path
[09](09-fault-and-kill.md) walks for any user fault, so the init is told through its watch. The
bring-up failure in Stop 4 (`kos_spi_bus_open` refused) takes that road, and so does the end of
`serve_loop`.

The kernel's teardown of the dying driver closes every capability it held ([09, Stop
7](09-fault-and-kill.md)). Two of them matter to the client.

1. **A reply capability, if the driver died holding a request.** The `CAP_REPLY` arm of
   `obj_close_protocol` finds the parked caller, sets its result to `-KOS_EPIPE` and wakes it.
   EPIPE means "the server died holding your request": the client knows the transaction may have
   half-run.
2. **The endpoint capability with `WAIT`.** It was the only receive-bearing capability (Stop 3:
   the init narrowed its own away). `endpoint_rights_dropped` takes the receiver count to zero,
   marks the endpoint vacated and calls `refuse_senders`: every caller parked waiting for the
   driver is answered at once with what a new caller would get. `endpoint_unserved` decides what
   that is. The init still holds `KOS_CAP_HANDOUT`, so a receiver may come back: `-KOS_EAGAIN`.

Then the init wakes. `Walk::wake` sees `KOS_TASK_DEAD`, kills the task to reclaim it, and calls
`Walk::death`. This composition sets no `restart:` on the driver, so its `restart_max` is 0: the
task goes to the `GONE` phase, and `Walk::drop` closes the init's own capability on the endpoint.
That was the last `HANDOUT`. From now on every call answers `-KOS_ECONNREFUSED`: nothing can
serve this endpoint again. `Walk::propagate` would mark a task still waiting to start on this
service as `DOWN`; the client is already running, so it learns from its calls instead.

What the client sees, through the class, is the kernel's code unchanged: `reply_payload` returns
`rc` when it is negative. So `kos_spi_transfer` returns `-32` (`-KOS_EPIPE`) for the call in
flight, and `-111` (`-KOS_ECONNREFUSED`) afterwards. `read_byte_test` passes it up, the client
prints `[k64dspi] BYTE_TEST attempt N: (xfer ERR rc=-32)`, then
`[k64dspi] LAN9252 BYTE_TEST FAIL: a transfer failed`, and exits 1, which ends the system.

With `restart: { max: 2 }` on the driver task, `Walk::death` puts it in `PENDING` instead, the
init keeps the endpoint, and the next `Walk::scan` calls `k64dspi_spi_start` again with the same
endpoint. Between the death and the new receive, callers get `-KOS_EAGAIN`, which tells them a
retry may work. The new start muxes the pins again and the new driver thread reopens the
controller; the old thread's window went back to the kernel when it died. What does not survive
is the service's state: the new thread's `SlotTable` is empty, so the client's next transfer on
its old device handle is answered `-KOS_EINVAL` (no device handle for this slot) until it opens
the device again. A restart keeps the endpoint, not what the service knew.

Note one hazard the class header states plainly: `kos_call` has no deadline. A driver that stops
replying *without dying* (a wedged pump loop, say) leaves its client parked forever. The two wake
paths above both need the driver's capabilities to be closed, which only its death does.

**Try it:** on a board, start the system, `break k64dspi_transfer`, `continue` to the first
BYTE_TEST, then make the driver fault in place with `set $pc = kickos::driver::trap` and
`continue`. Expect the client to print `xfer ERR rc=-32` and the FAIL verdict.
<!-- unverified: needs a FRDM-K64F -->

Statically, the driver's exits in the image: `bus_thread` calls `trap` after a failed bus open and
after `serve_loop` returns.

```
$ arm-none-eabi-objdump -d -C --no-show-raw-insn $PROXY --start-address=0x1d94 --stop-address=0x1e28 | grep -E '\bbl\b'
    1dae:	bl	1edc <k64dspi_bus_open>
    1db8:	bl	25fe <kos_print>
    1dbc:	bl	29ec <kickos::driver::trap()>
    1dc2:	bl	25fe <kos_print>
    1dea:	bl	24f0 <kos_reply_recv>
    1df8:	bl	2210 <k64dspi_bus_close>
    1e16:	bl	1c10 <kickos::spi::serve_one(kos_spi_bus*, kickos::spi::SlotTable&, unsigned char*, unsigned int)>
```

(The address range is `bus_thread`'s: `nm -C $PROXY | grep bus_thread` gives its start. There is
one `trap` call: after `k64dspi_bus_close` the code branches back to it. Between `kos_reply_recv`
and `k64dspi_bus_close` sit `adds r3, #14` and a `beq`: that is `serve_transaction_failed`, the
`-KOS_EFAULT` test of Stop 11, which loops back to receive instead.)

**Why, and the contract:** [Reference: ipc-call-reply, the death matrix](../reference/ipc-call-reply.md),
[09, Stops 7 to 9](09-fault-and-kill.md), [Book: Synchronous call/reply](../book/synchronous-call-and-reply.md)

## The same contract on another engine

The XMC4800 Relax Kit has a bus service of the same shape: `xmcssc`
(`system/driver/xmc4800/xmcssc/xmcssc.cc`, engine `spi_usic.cc`), with an application built both
ways from one source (`user/apps/xmc4800-relax/xmcssc/`, `systems/proxy.yaml` and
`systems/local.yaml`). Its `bus_thread` opens the bus through the same class, calls the same
`spi::serve_loop`, and is checked by the same `spi::desc_ok`; a client linked with the proxy sends
it the same bytes. What differs is all below the class. The engine drives a USIC channel in SSC
mode, not a DSPI. It is **interrupt-paced**: its descriptor claims a line (`LINES irq:edge`) and
gives the thread a notification and the line capability beside the endpoint, so the engine sleeps
in a notification wait between FIFO events, the road [07](07-interrupt-to-a-driver.md) walks. Its
chip select is the controller's own `SELO0`, so it accepts `KOS_BUS_CS_HW` and refuses
`KOS_BUS_CS_GPIO`, the reverse of the K64F. And its protection unit covers devices, so its
composition needs only `accepts: [coarse_gate]` (USIC0's two channels share one gate, and the
kernel's console is channel 0): the window there is enforced per thread. Same wire, same loop,
same client; a different engine and a different isolation ceiling.

## Where to next

- [05 - Call and reply](05-call-and-reply.md): the rendezvous under every bus call.
- [07 - An interrupt to a driver](07-interrupt-to-a-driver.md): the interrupt-paced road the
  XMC4800 engine takes.
- [08 - Spawn and caps](08-spawn-and-caps.md): the spawn and the delegated capabilities of Stops 3
  and 5.
- [09 - Fault and kill](09-fault-and-kill.md): the teardown behind Stop 12.
- [Reference: bus-service](../reference/bus-service.md): the full wire contract, the DSPI0
  register map, and the neutrality notes (where the two engines differ).
- [Book: Peripheral isolation and the hardware
  ceiling](../book/peripheral-isolation-and-the-hardware-ceiling.md): why the K64F needs
  `accepts` at all.

## Exercises

1. **Give the driver restarts.** Copy `proxy.yaml`, add `restart: { max: 2 }` under the driver's
   `priority: 12`, and run `kickos_compose admit` on it as in Stop 1. Predict whether admission
   accepts it (it does), then predict what `restart_max` the emitted table carries and what a
   client calling during the gap sees (Stop 12).
2. **Two holders.** In a copy of `proxy.yaml`, give the client `devices: [/dev/dspi0]` too (with
   the same `accepts`). Predict the admission's answer before running it. It refuses, with a
   line ending in ``ownership.device: task `client` holds `/dev/dspi0`, which task `k64dspi`
   holds on line 13``: a device window has one holder, whichever road its owner takes.
3. **Count the copies.** For the 7-byte BYTE_TEST, count every copy of the payload bytes in the
   service build: in the proxy, in the kernel, in `serve_one`, back. Then count them in the local
   build. Check your count against the `mem_copy` calls in `objdump` of both `kos_spi_transfer`s.
