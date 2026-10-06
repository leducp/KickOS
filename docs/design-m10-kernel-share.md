<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M10.1 -- the kernel share

> **Status: ACTIVE.** Sections 1 to 7 state the landed kernel contracts of M10.1.1 and
> M10.1.3 to M10.1.8.
> Section 8 lists the ABI headers. Section 9 states the x86 floor of M10.1.9.
> `roadmap.md` assigns the milestones and carries the rulings;
> [`design-m10-composition.md`](design-m10-composition.md) is the userspace design these
> mechanisms serve.

## 1. x86_64 address spaces on q35 (M10.1.1)

On q35, `CHIP_Q35` selects `HAS_ASPACE`. Each task has its own address space;
`KICKOS_MEMORY_ENFORCED` is on, and ring 3 cannot reach the kernel image, translation
structures or per-core block.

### 1.1 Address layout and frames

`aspace_init` adopts the firmware root. Its mapped slots form the supervisor-only kernel
half, shared by task roots; slot 0 stays unavailable to users. Empty slots from slot 1
up form the user half. `aspace_user_lo` supplies the offset U between a physical frame
and its user address. Boot derives U from the firmware root and paging depth, then
checks that the app image and frame pool fit in unmapped user slots without overflow.
The other translating backends answer U=0 through the same arch hook.

The kernel keeps its identity-mapped view of frames. `aspace_reserve`, self-grants and
user stacks use physical address + U in a task; the kernel accesses user memory through
`arch_aspace_acquire`, never by dereferencing that user address. All tasks use the same
U, so a delegated block has the same virtual address in each. Section 6 defines
separate placement for explicit windows.

The frame pool comes from the UEFI arena after early kernel allocations, including
idle's stack. `frame_pool_init` takes the remaining arena; `arch_ram_alloc` refuses
further allocations from it. Task roots and page tables come from this pool.

### 1.2 The split PE image

`pe_image.ld` places app code and data in page-aligned `.apptext`, `.appro`, `.appdata`
and `.appbss` sections, apart from kernel, arch and chip code. Each section and
`.kosinit` has a guard word so an empty section retains correct bounds under
`ld -m i386pep`. Kernel runtime copies and code that refers to app storage use the
split-image paths in `kernel/CMakeLists.txt` and `aspace_image_alias`.

The app-side `arch_syscall` body lives in app text; kernel callers use
`karch_syscall`/`karch_syscall64` in kernel text. No privileged thread runs app-half
code. User entry and return pointers gain U; kernel references that need the loader
address keep it. `tests/static/check_x86_64_app_split.py` and its allowlist enumerate
kernel-to-app references and reject app-to-kernel instruction or absolute-data
references. The app uses PC-relative references and passes the no-GOT check.

Firmware applies PE `DIR64` relocations to the loader address. Before the first task
image is seeded, `app_relocate` adds U only to absolute words *located in app storage*
and *targeting app code or data*. Kernel-located words stay at the loader address;
app-to-kernel words are rejected. The pass also covers constructor and destructor
arrays, temporarily clearing CR0.WP to update read-only pages and restoring it before
any task runs. A retained, supervisor-readable relocation directory is checked
against the PE directory and live input-object relocations; missing, orphan, unknown
or malformed records fail the build or boot. The image links twice so the retained
copy has the final directory's exact size. A zero-fixup app is valid only when both
record sets are empty.

The linker supplies loader-address app bounds and load delta. The x86 arch hook adds
U for user mappings; resolving a loader frame uses `app_va + load_delta - U`.
`aspace_image_seed` copies app data after relocation, so every task inherits the
converted pointers.

### 1.3 Isolation, multicore and cost

`ring3_init` grants no user access to the kernel half. User spaces map only their app
image and granted memory. Kernel root entries are shared; an unmap shoots down cores
holding the affected root before the frame is reused. The AP trampoline remains in
the kernel half.

A switch between spaces writes CR3. PCIDs and global kernel leaves are off, so this
flushes translations, including the kernel half. Creating a space copies kernel root
entries, maps app text and copies app data. PCIDs, global leaves and randomized
placement are separate later decisions; port grants and kernel-placed windows belong
to sections 7 and 6.

## 2. The task-creation authority and a 32-bit authority word (M10.1.3)

**Creating a task requires authority bit 6 in a 32-bit authority word**, whether through
`KOS_SYS_TASK_CREATE` or an implicit task created by spawn.

- **The word is 32 bits wherever it appears**: `kos_thread_params::authority`,
  `Thread::authority`, `CapAuthority` and its mirror `kos_cap_authority` in
  `user/include/kickos/sys/abi.h`, the mask `kos_cap_narrow(KOS_CAP_AUTHORITY, mask)` takes, and
  `kickos_app_authority()` with `KICKOS_APP_AUTHORITY` in `system/include/kickos/sys/init.h`.
  In `kos_thread_params` it moves beside `core_mask`, the other 32-bit word, and `cap_count`
  moves behind `privileged`, keeping the struct's size. Spawn rejects undefined authority bits;
  `kos_cap_narrow` intersects its mask with held bits and cannot grant new ones.
- **The tasks authority** gates both ways a task is born:
  `KOS_SYS_TASK_CREATE`, and the implicit task `task_for` builds inside `thread_create_call`
  (`kernel/syscall/syscall_thread.cc`) when a spawn brings its own `mem_base` or changes
  privilege.
- **A thread spawned into the caller's task** needs no task authority. Seating a thread in
  a task the caller created is governed by creatorship. Root's `CAP_AUTH_ALL` and the fallback
  app authority include the bit, matching `main`'s default `memory`, `system` and `tasks`.
- **The authority is a bit** because the other system powers use the same word at spawn and
  are narrowed by `kos_cap_narrow`; a capability would add an object kind and table slot.

## 3. Handing out receiving, and the errno split (M10.1.4)

**An endpoint whose server is being restarted answers "try again", and one that will never be
served again answers "refused".** `Endpoint::recv_holders` counts WAIT-bearing capabilities;
`handout_holders` counts capabilities that can delegate WAIT without receiving.

- **The handout right** lets its holder delegate WAIT without holding WAIT
  itself. It is the only right a delegation may turn into another, and only into WAIT. It fits
  the entry: three rights bits plus the four of the call sequence's high half leave one spare bit
  in `CapEntry`'s byte, and the staged grant byte packs four type bits beside four rights bits.
- **`Endpoint::handout_holders`** counts the capabilities carrying it, a byte beside
  `recv_holders`. `endpoint_create` gives the creator all four rights, counted in both.
- **`kos_cap_narrow` narrows any capability**, not only the authority word; that is how an init
  drops WAIT and keeps HANDOUT, and a narrow that drops the last WAIT runs the same wake a close
  does.
- **The split.** With no local receiver, a send or call answers `-KOS_EAGAIN` while a handout
  holder exists and `-KOS_ECONNREFUSED` once none does, and parks in neither case.
  A sender parked when the last receiver leaves is woken with the same choice. The last
  receiver leaving marks the endpoint vacated, and a receiver counts only once it has waited on
  it since, so a server a handout seats leaves callers answered `-KOS_EAGAIN` until it first
  receives; a fresh endpoint's creator counts from the start. `-KOS_EPIPE` keeps
  exactly one meaning, the server died holding the request, from the reply arm in `cap.cc`. The
  call path's refusal of a dying receiver before it took the request is the no-receiver case.
  The fast path (`kernel/syscall/syscall_ipc_fast.cc`) falls through to `endpoint_call` for
  this decision.
- **Clients** retain their endpoint capability on `-KOS_EAGAIN`; they close it on
  `-KOS_ECONNREFUSED`. Far AMP endpoints keep their existing codes because the wire carries no
  errno and a far node has no local receiver to count.
- **Publishing the console.** `kos_console_publish` takes a capability holding HANDOUT on the
  endpoint and refuses `-KOS_EACCES` for one without it, a WAIT-only capability included. It
  seats WAIT on that capability when it does not hold it already, counted, and the endpoint
  receives as a fresh one does for its creator, so every publish leaves the publisher holding
  WAIT and HANDOUT. The creator's own capability carries all four rights, and the init, which
  keeps a console driver's endpoint with HANDOUT once the first handover ends, publishes it again
  at every restart and repeats that handover: its narrow at the end drops the WAIT the publish
  seated, which is the last one going, and the kernel's console coming back, when the start
  failed before its receiver held one. `docs/reference/console.md`, "Capability handover", states
  the contract.

## 4. Deaths and first receives, reported to the creator (M10.1.5)

**A creator learns, on a notification it names, when a task it created has died and when it has
become ready.** The creator names the endpoint whose first receive marks readiness.

- **`kos_task_watch(task, notify_cap, ready_ep)`** arms both reports on a task the
  caller created (`-KOS_EPERM` otherwise, through `task_created_by`). A capability handle means
  something only in its holder's table, so the task records the OBJECTS: the notification by
  generational object handle, with the capability's badge as the bits to raise, and the
  endpoint whose first receive means ready, or none for a task that serves nothing.
- **The watch names the notification, and holds no reference.** It resolves the object by
  generational handle at each raise, so a notification freed and its slot seated again is never
  raised into. Whoever can receive the report holds a capability or a binding, and either keeps
  the object alive, so the creator closing its own capability changes nothing for them; a
  report nobody can receive goes nowhere. The watch ends when the watched task's slot is
  freed, or the creator re-arms it or disarms it by passing no notification. The endpoint is
  held the same way, compared only.
- **Death** raises the bits once, when the task's last member has exited and `cap_teardown` has
  run, which is the point `WAIT_TASK_EMPTY` wakes a slayer. By then everything its threads held
  is free -- their stacks, their windows (section 5 makes a window outlive no holder), their
  capabilities and the lines they claimed. The task itself is not: an explicit task is reserved
  by its creator until the creator releases it, which is what lets `kos_task_state` still answer
  for that instance and keeps its handle from being reused under the creator.
- **A restart releases the dead instance first.** The init reads the empty state, releases its
  hold with `kos_task_kill`, which on an empty task cancels nothing and frees the slot and its
  domain, and only then creates the new task. The kernel lifecycle does not change; the step is
  the init's, in `design-m10-composition.md`'s walk.
- **Ready** raises the bits once, in `endpoint_recv_locked`, the first time a member waits on the
  armed endpoint. Every receive reaches it, the fast path serving calls only. The receive has not
  parked yet, so the creator's wake joins the receive's deferred wakes and runs once it has.
- **`kos_task_state(task)`**, a creator-only call, answers whether that instance has members,
  whether it has become ready and whether it is dead, and `-KOS_EBADF` once its slot is freed.
  Ended and dead are section 10's: dead, not the absence of members, is the death. A
  notification's bits merge a death and a ready into one wake, so the init reads the state of
  each task the badge names, as `design-m10-composition.md` requires.
- **The generation is the task handle's.** A restart is a new kernel task and a new handle, whose
  generation (`Task::gen`) differs from the dead one's, so the instance a ready event belongs to
  is the handle the init holds, and no counter is added.
- **Nothing names the init.** The report goes to whoever armed it, which only the creator can,
  so a nested init's children report to it.

## 5. A window list in spawn (M10.1.6)

A spawn carries device, memory and port windows through `kos_thread_params::windows` and
`window_count`. Each `kos_window` has `base`, `size`, `kind` and `flags`; the flags specify
read-only and uncached access. `mem_base` remains the task's own data region and determines
whether spawn creates an implicit task.

- **Capacity.** A thread holds at most `KICKOS_MAX_THREAD_WINDOWS` entries. Its default and
  ceiling are four: an eight-region MPU spends four on code, static data, task data and stack.
  M10.3 exports the limit for offline admission. Excess entries return `-KOS_ENOMEM`.
- **Possession.** The holder is a thread. Its region list (`Thread::mpu`) records device
  windows. A sibling on a translating board may reach the task-space mapping, but exclusivity
  and peripheral syscalls check the holding thread. A self-grant cannot confer device type.
- **Lifetime.** Map when the holder is created; unmap when it exits, before releasing the
  window and waking a supervisor. A multicore unmap completes its shootdown first. A dying
  holder counts for exclusivity until no thread holds and no space maps the window.
- **Admission.** Device windows are exclusive across threads and within one list, and accept
  no flags. A memory window must lie within a block reserved by the spawner and pass the
  self-grant checks. Peripheral syscalls accept any device window held by the caller. Port
  windows return `-KOS_ENOTSUP` until section 7; translating boards refuse memory windows
  until section 6 defines their placement.
- **Read-only overlap on region boards.** If any region covering a user range is read-only, neither
  `user_range_ok` nor `user_readable_and_writable_ok` permits writing that range. This holds
  even when another window over the same block is read-write.

## 6. The kernel chooses where a window sits (M10.1.7)

**On a translating board the kernel maps each device and memory window at an address it chooses
in the task's space, and `kos_window_get` answers it by the window's place in the spawn list.**
Today no translating board maps a device window at all: `arch_mpu_region_encodable` answers false
in `arch/arm64/armv8a/arch_armv8a.cc`, `arch/riscv/rv64imac/arch_rv64imac.cc` and
`arch/x86/x86_64/arch_x86_64.cc`, so a spawn carrying a window there is refused. This part is the
first device mapping in a user address space, not a move of one.

- **Where.** Each translating backend states a window area in its user half, clear of the
  addresses its RAM and app window occupy, and `kernel/mem/vrange.cc`'s `VirtualRanges` places
  each window first-fit in it. Randomising the choice is the later policy the composition design
  separates from choosing.
- **How.** Device windows map with `ARCH_MAP_DEVICE`, which the armv8a and x86 map editors
  honour; rv64 has no memory type in its page tables and maps them as it maps any page. A
  holder's exit unmaps its windows, section 5's rule, and on a multicore preset the editor's
  existing shootdown invalidates the cores holding the space before the window is released.
- **What stays.** `mem_base`, `stack_base` and blocks from `kos_ram_alloc` keep today's
  placement; the `kos_ram_alloc` comment in `user/include/kickos/sys.h` stops promising that a
  delegated block keeps its address, which the ruling says was never an invariant.
- **`kos_window_get(index, &window)`** answers the window at `index` in the calling thread's
  spawn list, by the place recorded with it at the spawn: its base where the thread reaches it,
  its size, kind and flags as spawned, a memory window's size rounded up as its block was, and
  `-KOS_EINVAL` past the list. A window is asked by its place and never by its base, the base of
  a memory window being the spawner's reservation, which the child cannot know. On a region board
  and on the sim the base is the physical one, the only address there is, and a port window's is
  its first port.

**Arms.** On each translating preset, the address answered differs from the physical one and the
device reads through it (the PL031 on `virt`); a place past the caller's list is refused; the
mapping is gone when its holder exits while a sibling survives, the sibling faulting on it, and
a second task is refused the device until then; the mapping is made again for the next instance; a region board answers the
physical base.

**Backends and cost.** Portable kernel, armv8a, rv64 and x86_64. A page map per window at spawn,
an unmap and an invalidation at the holder's exit, and a mapping record per window in the task's
ranges.

**What the part taught.** The record is a `VR_WINDOW` range naming its holder, a thread slot, its
place in the holder's spawn list, and for a memory window the donor domain it holds a reference on.
The holder and donor fit the padding of `VirtualRange`, and the place shares the word a capability
mapping's frame run takes, no range being both, so neither the list nor the thread control block
grows. A region board records each window's place beside its region in the region set, two bits
a region, and x86 beside each range of the context's port set (section 7): the place is the
window's index in the spawn list, recorded at the spawn and never derived from where a store
keeps the window, a store compacting or reusing its slots. The window area is 1 GiB
halfway up each user half (`arch_aspace_window_area`), far above RAM's user alias. A memory
window names one reservation of the spawner's own, whole, and its entry holds the spawner's
domain so the frames outlive a spawner that dies first; a device window keeps its possession
record in the region set, at its physical base, which the pointer checks now skip where a space
translates. Admission needed a rule the section did not state: no translating chip declared the
devices it owns beyond its interrupt controller, so a device window could have named RAM, the
PLIC, an ECAM or the I/O APIC. Each translating chip now states the apertures a user window may
lie in (`arch_window_apertures`) -- the PL011 and PL031 on arm64 `virt`, the goldfish RTC and
the console UART on rv64 `virt`, the HPET on q35, none on the i.MX 8M Plus -- and the reserved
blocks still carve the kernel's own out of them. The arms are `window_addr`, on each translating
preset, a holder in a task of its own reading the spare device through the answered address,
answered nothing past its list and refused a window outside every aperture, a second
task refused the device while it lives, its sibling faulting on the address once it has exited,
and the next instance mapping it again; `window_memory_ro` now runs on the translating presets
too, its children reaching the block where `kos_window_get` says and reporting over an endpoint,
a task's data being its own there; `window_list` on the sim, which answers the physical base; and
`window_get`, lists of a device window, a memory window and on x86 a port window in more than one
order, each spawned once the one before has given its ranges back, every window answered at its
place with its kind, size and flags on every region, sim and translating preset.
Removing the unmap at a holder's exit or answering the physical base turns them red. Three
rules hold besides. A memory window, a self-grant and a task's data handoff take no memory type
another live mapping of the same frames does not carry, so a block mapped cacheable is never lent
to a window uncached (`aspace_frames_type_ok`, `memory_type_free`,
`-KOS_EBUSY`), a window asking again where it is mapped, after the spawn's own data, and a
region board asking the held task domains beside the live threads, an empty task holding its data
with no member; a list names a block once; and the region set marks its windows, so a region board
answers none for its stack or data region. `window_memory_ro` witnesses all three. The spawn
chain's frame sets the x86 reservations, at one core and above: SYSK 1832 and 2432, SYSPRIV and
SYSPRIVSW 1856 and 2368, and the spawn floor 2624 and 3328.

## 7. The x86 port grant and `kos_port_reg_write` (M10.1.8)

**A task on q35 reaches exactly the ports it was granted, and writes a privileged port only
through the kernel.** Today `build_tss` (`arch/x86/x86_64/desc_x86_64.cc`) sets `iomap_base` to
`sizeof(tss64)`, meaning no bitmap, and IOPL is 0, so ring 3 reaches no port. Section 1 makes the
task-state segment unreachable from ring 3, which is what lets a bitmap in it be a boundary.

- **The bitmap.** Each core's task-state segment carries the full 8 KiB bitmap and its terminator
  byte, with no ceiling on the port number. A switch compares the incoming thread's port set with
  the one loaded and, only when they differ, closes the outgoing ranges and opens the incoming
  ones, a few bytes for a device such as the CMOS clock. The comparison sits beside
  `publish_current` in `arch_switch`.
- **Kernel-owned ports.** q35 states the ports the kernel keeps, as `arch_reserved_blocks` states
  memory: the interrupt controllers, the timer, the ACPI power control, PCI configuration, the
  debug exit, and COM1 while it is the console. A port window overlapping one is refused, and a
  second holder is refused as for a device window.
- **`kos_port_reg_write(base, offset, value)`** writes one byte to `base + offset` if the caller
  holds a port window covering it and the port is on the chip's allowlist with the value inside
  its mask. The CMOS index is the first entry: its mask withholds bit 7, the NMI mask. An
  allowlisted port is never opened in a bitmap. Off x86 the call answers `-KOS_ENOSYS`.
- **The ports kind** of section 5 stops being refused on this board.

**Arms.** A granted data port reads; a port outside the grant faults the task, as
`kickos_x86_64_probe_outb` does in the bring-up probes; a direct write to the CMOS index faults,
the kernel write succeeds, and a value with bit 7 is refused; a second holder and a kernel-owned
range are refused; two tasks alternating on one core each reach only their own ports; the same
after a migration on the two-core preset.

**Backends and cost.** x86_64 and q35, and a portable seam answering `-KOS_ENOSYS` elsewhere. One
compare per switch, a few bytes written when the port set changes, 8 KiB per core. M10.1's exit
record lands with M10.1.10, the last part, so that it measures what M10.1 ships.

**What the part taught.** A thread's port windows are its possession record, kept in the x86
`arch_context` (`ports`, `port_count`) because the switch sees only the context: `publish_current`
hands them to `tss_load_ports`, which compares them with the set the core has loaded and rewrites
the bitmap only when they differ. `port_places` holds each range's place in the spawn list, two
bits a range, which is how `kos_window_get` finds one. The
kernel-owned list became apertures, as the windows of section 6 did: q35 states the ports a window
may name (`arch_port_apertures`), the CMOS pair and COM2, because a list of what to refuse would
have to name the DMA controllers, which write memory, and the fast-reset port as well as the
kernel's own. The allowlist is the PC platform's rather than q35's (`port_reg_mask` in
`desc_x86_64.cc`), the kernel-free probe images linking the bitmap without the chip. A port window
maps nothing and a holder's exit drops it with its device regions. The arm is `port_window`: the
CMOS holder's kernel write, its data read, the refusals of bit 7 and of an unheld port; a second
holder, COM1 and the PIC refused; a COM2 holder alternating with it on one core and faulting on the
CMOS data port; the CMOS holder moving itself to core 1 and reading there on the two-core preset, a
creator having no right to pin another task's thread; and a direct index write faulting. Removing
the close on switch, opening the index, dropping the holder check or loading every switch into core
0's bitmap turns it red. The switch's compare lies on the interrupt chain, which x86's single-core
IRQ and IST reservations of 576 and its spawn floor of 2624 cover
(`arch/x86/x86_64/include/kickos/arch/x86_64_trap_stack.h`); rv64 reserves its SYSK, a spawn
seeding the child's tables, at 2048.

## 8. ABI touchpoints

Sections 2 to 7 list landed names. The headers are canonical.

```c
/* user/include/kickos/sys/abi.h */
KOS_AUTH_TASKS = 1 << 6,                 /* 2: kos_cap_authority, mirrored by AUTH_TASKS */
uint32_t authority;                      /* 2: kos_thread_params, beside core_mask */
KOS_CAP_HANDOUT = 1 << 3,                /* 3: kos_cap_rights */
KOS_SYS_TASK_WATCH,                      /* 4: (task, notify_cap, ready_ep) -> 0, or -KOS_E* */
KOS_SYS_TASK_STATE,                      /* 4: (task) -> enum kos_task_state bits, or -KOS_EBADF */
struct kos_window {                      /* 5 */
    uintptr_t base; uint32_t size;
    uint8_t kind;                        /* KOS_WINDOW_DEVICE, KOS_WINDOW_MEMORY, KOS_WINDOW_PORTS */
    uint8_t flags;                       /* KOS_WINDOW_RO, KOS_WINDOW_UNCACHED */
};
struct kos_window const* windows;        /* 5: kos_thread_params, replacing mmio_base, mmio_size */
uint16_t window_count;
KOS_SYS_WINDOW_GET,                      /* 6: (index, out) -> 0, or -KOS_EINVAL past the list */
KOS_SYS_PORT_REG_WRITE,                  /* 7: (base, offset, value) -> 0, or -KOS_E* */

/* system/include/kickos/sys/errno.h */
KOS_ECONNREFUSED = 111,                  /* 3 */

/* Kconfig */
KICKOS_MAX_THREAD_WINDOWS                /* 5: a thread's window bound, from the region budget */
```

| header | part | change |
| --- | --- | --- |
| `user/include/kickos/sys/abi.h` | 2 | `kos_thread_params::authority` 32 bits and beside `core_mask`; the tasks bit; `kos_cap_authority` widened |
| | 3 | the handout right; `KOS_SYS_CAP_NARROW` narrows any capability |
| | 4 | the watch and state syscalls and the state bits |
| | 5 | `struct kos_window`, its kinds and flags; `windows` and `window_count` replace `mmio_base` and `mmio_size` |
| | 6 | the window-by-place syscall |
| | 7 | the port-write syscall |
| `system/include/kickos/sys/errno.h` | 3 | the refused errno, 111; `KOS_EPIPE` and `KOS_EAGAIN` reworded |
| `user/include/kickos/sys.h` | 3 to 7 | the wrappers `kos_task_watch`, `kos_task_state`, `kos_window_get`, `kos_port_reg_write`; the `kos_ram_alloc` comment |
| `user/include/kickos/kos.h` | 2, 5 | `kos::create`'s authority and window arguments |
| `system/include/kickos/sys/init.h` | 2 | `kickos_app_authority` and `KICKOS_APP_AUTHORITY` 32 bits |
| `user/include/kickos/sys/driver_service.h`, `emit.h`, `driver/spi.h`, `driver/i2c.h` | 3 | the no-receiver answers |
| `user/include/kickos/sys/abi_probe.h` | 3 to 7 | probe selectors the new arms need, selftest only |

`arch/include/kickos/arch/arch.h` changes too -- the window area of section 6, the port seams of
section 7 -- and is the kernel's internal interface, not the ABI.

## 9. The x86-64-v3 floor (M10.1.9)

**x86 is compiled for x86-64-v3 and refuses a processor below it**, so that no older part is
maintained. The level is the psABI's third, the Haswell and Excavator generation. The vector units
stay off, `-mno-sse` keeping AVX out of the code as before, so what the level gives the kernel is
the integer extensions, BMI1 and BMI2, LZCNT, MOVBE and POPCNT.

- **The refusal.** `efi_main` first calls `kickos_x86_64_floor`
  (`arch/x86/x86_64/floor_x86_64.cc`), which is compiled for the base level, because below the
  floor the first instruction the level adds is an invalid-opcode fault nothing reports. It reads
  the level's CPUID bits and, on a processor short of any, writes a line to firmware's console and
  returns `EFI_UNSUPPORTED`, so the boot manager moves to its next option. The file refuses to
  compile if the extension macros say it was built for the level.
- **The emulated processor.** `qemu64` plus the level's features, stated once as
  `KICKOS_X86_64_QEMU_CPU` in `cmake/kickos.cmake`. It reaches the application gates through
  `tests/lib/gate.sh` and the boot witnesses through their runners as `KICKOS_X86_64_CPU`, which
  the runners now require. The SMP presets add x2APIC, as before.
- **The deletion.** SYSCALL is a base-level feature, so `ring3_init`'s probe for it goes. The probes
  for the execute-disable bit, the local APIC, PAT, PCID, INVPCID and MAXPHYADDR stay: the level
  does not name them, and an Excavator is v3 with no PCID.

**Arms.** `x86_64_x1_floor` boots the handover image on plain `qemu64` and requires the refusal and
no handover line. Deleting the call turns it red, and dropping the base-level option stops the
build.

## 10. A task's end, its status, and the window by its place (M10.4.3)

**A task ends with the thread it was started for.** Its entry is the first member a non-member
seats in it, or the thread whose spawn built an implicit task (`Thread::task_entry`, set at spawn).

- **Ended.** The entry's own exit, by returning or through `kos_exit`, or any member's fault ends
  the task as a process ends: the kernel sets `KOS_TASK_ENDED`, raises the creator's watch, and
  stops every other member at once with `CANCEL_SLAY`, whether or not a creator holds the task,
  latching the status unless the ending thread was itself cancelled. No member runs a further
  instruction of its own, one that never enters the kernel included. An entry that was itself
  cancelled ends nothing, its canceller's verb having stopped the group. A non-entry member's
  exit ends that member alone.
- **Killed.** `kos_task_kill` stops every member the same way, a member spinning without a system
  call included, and drops the creator's hold without waiting; `kos_task_slay` stops them and
  waits until every member is swept.
- **No new member.** An ended task refuses every spawn into it with `-KOS_EBUSY`, live members or
  none: a restart is always a new task.
- **Dead.** `KOS_TASK_DEAD` is set, and the watch raised again, once every member's capability
  sweep is done: each member counts in `Task::sweeping` from its release to the end of its sweep,
  and the one that brings an empty task's count to zero reports the death. The same member ends a
  `kos_task_slay` waiting on the task, which therefore returns, and frees the slot, only once the
  task is dead; it answers at once only for a task with no member and none sweeping. A slot is
  seated again only once no member sweeps under it, so a restart into it counts no capability
  of the old instance in its budget.
- **The status.** `kos_task_exit_status(task, &status)` answers the creator once the task has
  ended: the entry's code, 0 when it returned, `KOS_EXIT_FAULT` after a fault, or
  `KOS_EXIT_CANCELLED` when every member was cancelled or the end was a cancelled thread's, in an
  `int` so a negative code is not an error.
- **A window asked by its place.** `kos_window_get` (section 6) answers by the place each store
  records at the spawn, with the flags the window was spawned with even after a self-grant retypes
  its region.
- **Reservations.** The entry flag in `spawn_masked`'s frame and the explicit task's seed
  posture (`DOM_CALLER_TASK`) carried through the domain claim are on the spawn chain. x86
  reserves SYSK at 1832 on one core and 2432 above it, SYSPRIV and SYSPRIVSW at 1856 and 2368,
  and the spawn floor at 2624 and 3328
  (`arch/x86/x86_64/include/kickos/arch/x86_64_trap_stack.h`, `Kconfig`).
- **Data from the image.** On a translating board an explicit task's space copies its static
  data from one snapshot of root's, taken by the first explicit task's seed (`DOM_CALLER_TASK`),
  so every task and every restart an init starts begins from the image's data as root held it
  then and never from a global the init wrote since. A spawn bringing its own data grant keeps
  copying root's live data while root lives, as the T6 template of `design-m6-mmu.md` states:
  a global root writes before such a spawn is one the child reads, which the selftest's IRQ
  driver arms rely on. A snapshot that cannot be taken refuses the creation with `-KOS_ENOMEM`.
- **Every reservation zeroed.** `kos_ram_alloc` clears a region board's block after the bracket
  that recorded it, and cleans it to memory and invalidates its lines where the arch puts a data
  cache over the arena, as the frame pool already clears a translating board's.

```c
KOS_SYS_TASK_EXIT_STATUS,   /* (task, int* status) -> 0, or -KOS_EBUSY / -KOS_EBADF / -KOS_EPERM */
KOS_TASK_DEAD = 1 << 2,     /* kos_task_state: empty, every member swept */
KOS_TASK_ENDED = 1 << 3,    /* kos_task_state: status set, no new member */
```

## 11. A thread sets its own priority (M10.4)

`kos_thread_set_priority(priority)` sets the calling thread's base priority, and only the
caller's: a thread names no other.

- **Range.** Outside `KICKOS_PRIO_MIN` to `KICKOS_PRIO_MAX` it answers `-KOS_EINVAL`, the whole
  argument word read, so a value whose low byte is in range is refused too. Priority 0 is idle's.
- **Ceiling.** Lowering is always allowed. Raising is allowed up to the calling task's priority
  ceiling, the half of `kos_task_sched_grant` that a spawn already checks, and past it answers
  `-KOS_EPERM`. A thread's base never sits above its task's ceiling, so the check refuses raises
  alone.
- **Inheritance.** The call writes `Thread::base_prio` and recomputes the effective priority
  through `thread_effective_prio`, the funnel every mutex unlock, reply and close uses. A thread
  boosted by a mutex waiter or a donating caller keeps the boost and falls to the new base once
  the boost ends. The caller is running, so it is parked on nothing and donates to no one: no
  boost it gave has to move with it, and the donation chain is untouched.
- **Rescheduling.** A change of the effective priority re-seats the caller at its new level,
  behind the threads already ready there. A lowering reschedules before the call returns, so a
  ready thread it lets outrank the caller runs at once; a raise lets nothing outrank it and
  takes no pass.

Root uses it. The kernel creates root at `KICKOS_PRIO_MAX` and root lowers itself: a system
target's init to its composition's priority as its first act, any other image to
`KICKOS_PRIO_ROOT` before the app's constructors
([`design-m10-target.md`](design-m10-target.md), section 1.1).

**Arms.** `prio_self_raise_lower`: a thread raises itself above a peer it readies, which stays
off the CPU, then lowers itself below it, which runs before the call returns. `prio_self_ceiling`:
a member of a task narrowed to a ceiling raises itself to it, is refused one past it, and is
refused 0, `KICKOS_PRIO_MAX + 1` and a wide word. `prio_self_boosted`: a mutex holder boosted by
a waiter lowers its base, keeps the boost while it holds the mutex, and falls below a thread its
old base outranked once it unlocks.

```c
KOS_SYS_THREAD_SET_PRIORITY,  /* (priority) -> 0, or -KOS_EINVAL / -KOS_EPERM */
int kos_thread_set_priority(uint8_t priority);
```
