<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M10.1 -- the kernel share

> **Status: ACTIVE -- written one section per part, each reviewed before its code.** Section 1,
> the x86 address spaces, is what M10.1.1 built. Sections 2 to 7 are the ABI changes of M10.1.3 to
> M10.1.8, section 8 lists every header they touch so the ABI is reviewed as one, and section 9
> is the baseline each part is measured against. Each part corrects its own section if its code
> teaches otherwise.
> `roadmap.md`'s M10 section assigns the numbers and carries the rulings;
> [`design-m10-composition.md`](design-m10-composition.md) is the userspace design these
> mechanisms serve.

## 1. x86_64 address spaces on q35

**On q35 every task gets its own address space, and the kernel becomes unreachable from ring
3.** Today neither holds: `CHIP_Q35` selects no `HAS_ASPACE` (`arch/Kconfig`), so
`KICKOS_MEMORY_ENFORCED` is 0 on this board and the split-image machinery the other translating
boards run is off here, and `ring3_init` in
`arch/x86/x86_64/ring3_x86_64.cc` sets the user bit over the whole image and the whole arena, so
an unprivileged thread reads and writes kernel text, kernel data, live translation tables and
the per-core block. That was a bring-up posture. A general-purpose x86 port without address
spaces is a defect (maintainer, 2026-09-28), and the port grant of M10.1.8 is a boundary only
once this section holds.

### 1.1 What is already there

- **The map editor is complete.** `arch/x86/x86_64/aspace_x86_64.cc` implements the whole
  `arch_aspace_*` family the armv8a and rv64 backends do. `aspace_init` adopts the firmware's
  root: every slot it maps (the identity map of low memory, this image and the arena inside it)
  becomes the kernel half, shared by every space through copied root entries and shared child
  tables; the user half is the empty low slots from slot 1 up (`aspace_user_lo`,
  `aspace_user_hi`), slot 0 staying unmapped for null. Memory types go through the attribute
  table (`aspace_memtype_bits`), an unmap on one core is shot down on the others by IPI
  (`invalidate_all`), and which cores hold a root is recorded (`g_residency`).
- **It has a registered witness**, `x86_64_x5_aspace` in `cmake/x86_64_boot.cmake`, which links
  a frame pool of its own (`arch/x86/x86_64/probe5_x86_64.cc`). An application image instead
  links a frame-pool decline, `nopool_x86_64.cc`, whose `kickos_frame_alloc` panics, so nothing
  an app runs has ever reached the editor. This section deletes it.
- **What is missing is the rest of the chip's half**, which a translating chip states in its
  linker script and this one cannot, firmware loading a PE32+ image with no KickOS script:
  the app's own window (`__kickos_app_rom_start` to `__kickos_app_sram_end` and
  `__kickos_app_load_delta`, which `aspace_image_seed` in `kernel/mem/aspace.cc` uses on other
  translating boards), and the frame pool (`__kickos_frame_pool_start`, `__kickos_frame_pool_end`,
  `__kickos_frame_pool_delta`, which `frame_pool_init` in `kernel/mem/frame_pool.cc` reads).
  `arch/x86/x86_64/pe_image.ld` links kernel and app into one `.text`, one `.data` and one
  `.bss`, and the chip ships no `aspace.cmake`, the opt-in the root `CMakeLists.txt` checks
  against the Kconfig select.
- **The kernel side of the split is already built and chip-independent.** Under
  `KICKOS_HAVE_ASPACE`, `kernel/CMakeLists.txt` gives the kernel private copies of the runtime
  (`kickos_privatise_runtime`), and builds the portable kernel sources that name the app half
  under the code model that reaches it (`kickos_split_image_tu`): `kernel/init/kmain.cc`,
  `kernel/thread/reent.cc`, `kernel/mem/aspace.cc` and `kernel/domain/domain.cc`. Those that
  touch app storage do it through `aspace_image_alias`. That list is a code-model list, not the
  set of sites taking an app address; section 1.3 says where that set comes from.

### 1.2 The one difference from armv8a: a user address is not a physical address

On armv8a and rv64 a task reaches a frame at its physical address: `aspace_reserve` maps a
frame run at `va == pa`, as do `aspace_self_grant` and the user stacks of
`kernel/mem/ustack.cc`. On x86 that address is in root slot 0, which is kernel half, and the
editor's `range_ok` refuses it. So:

**On x86 a task reaches every frame at its physical address plus one constant, the user
offset U**, which is `aspace_user_lo`, the base of the first user slot (512 GiB under 4-level
paging when slot 1 is free). `aspace_init` learns U from the firmware root and the active
paging depth; it is a boot-time value, not a linker constant. The boot check verifies that the
app image and the whole frame pool, after adding U, fit in absent user slots without wrapping
or crossing a slot the firmware mapped. It is zero on armv8a and rv64. The kernel's own view
stays the identity map, so the frame pool's delta is 0, and the kernel reaches user memory
only through `arch_aspace_acquire` (`access_copy` in `kernel/syscall/syscall_mem.cc`), never
through the user address.

- The places that assume `va == pa` read it from one arch hook instead, the offset answered by
  the backend, so there is one rule on every translating board with a constant that is zero on
  two of them.
- A block delegated to another task is still reached at the same address there, the offset
  being the same in every space. The `kos_ram_alloc` comment in `user/include/kickos/sys.h`
  stays true until M10.1.7 makes the kernel choose window addresses.
- The offset is fixed, so user addresses are as predictable as they are on armv8a today.
  Choosing and randomizing are separate decisions (`design-m10-composition.md`, "Where a
  window sits"), and this section takes neither.

### 1.3 The link: the app gets its own window

`pe_image.ld` groups the app's input sections into page-aligned sections of their own --
`.apptext`, `.appro`, `.appdata` and `.appbss`, the last holding the app's heap bounds, empty on
this board -- and the kernel keeps
the kernel, arch and chip archives (`libkickos_kernel.a`, `libkickos_arch_x86_64.a`,
`libkickos_chip_q35.a`). `libkickos_lib.a` is app code, as it is on every split-image board. The
bounds are the symbols the other translating chips state, and `appdata_no_kernel`
(`tests/integration/gates/selftest.cmake`), which excluded x86 only because its windows were
empty, checks this chip too. Its record rule reads plain objects as well as archive members,
because this board links its UEFI entry and kernel landing unarchived.

**An empty window section loses its bounds.** `ld -m i386pep` drops an empty output section
after assigning the symbols in it, and re-bases them onto the image's first section: an app
with no initialized data had `__kickos_app_sram_start` on the kernel's `.text`, which root's
space would then have mapped. Each window section, and `.kosinit`, therefore opens with a guard
word so it is never empty, and the link check refuses a script symbol whose value in the image
is not the one the map states.

**Three things cross the line, and each has one answer.**

- **Arch code the app executes goes in the app window by section, not by archive.**
  `arch_syscall` in `arch/x86/x86_64/switch.S` is the arch archive's, and every syscall stub in
  `user/src/syscall_stubs.cc` calls it. Its app-side body keeps the `%rcx` to `%r10` argument
  move, then executes `syscall; ret`, in a section the script places in app text, as rv64 does
  with `.apptrap`. The current body also branches
  straight to `syscall_dispatch` for a privileged caller; that branch cannot remain in app
  text, and it has no caller once this chip translates: the translating boards' rule is that no
  privileged thread runs app-half code (`docs/design-m6-mmu.md`), root being unprivileged,
  idle the one privileged thread, and `thread_create_call` letting only a privileged caller
  spawn a privileged child. Kernel callers get a separate `karch_syscall`/`karch_syscall64`
  body in kernel text, matching the split-image declarations in
  `arch/include/kickos/arch/arch.h`.
  `kickos_user_thread_return` is already app code in `user/src/syscall_stubs.cc`.
- **A stored app pointer needs a second relocation.** `-fpie` makes code references
  PC-relative, but the PE image has a nonempty base-relocation directory with `DIR64` words;
  `objdump -p` on today's x86 `hello.efi` shows them. Firmware rebases those words to the
  loader address, which is the kernel's view. Before the first image seed, the x86 boot path
  walks a retained, supervisor-readable copy of those records. A fixup *located in app
  storage* and *targeting app storage or text* gains U. A fixup located in the kernel stays at
  the loader address, including a kernel word holding an app symbol for an explicit conversion
  below. A fixup located in the app and targeting the kernel is refused by the link check.
  The pass covers app read-only data and initialized data, writes read-only pages with CR0.WP
  cleared for the pass alone and set back before any task runs, and checks the PE record type
  and bounds before writing. The app's constructor and destructor
  arrays live in app storage and receive the same conversion; `kickos_root_entry` then calls
  their aliased entries. The link preserves the relocation records in a non-discardable,
  supervisor-readable image section and checks that this retained copy matches the PE
  directory firmware uses. Boot refuses a missing or malformed copy rather than guessing.
  The linker writes `.reloc` last, so the copy's size is known only after a link: the image is
  linked twice, the first link measuring the directory and the second reserving exactly that
  much, which leaves `.reloc` unchanged because the retained section is placed after every
  section a fixup can sit in and carries none itself. A build check refuses a second link whose
  directory differs from the first's.
- **Kernel code that names an app symbol is audited.** The linker answers an app symbol at the
  loader address, not at its app address as on armv8a. The two addresses handed to a user
  thread gain U: `kickos_root_entry` in `kernel/init/kmain.cc`, and
  `kickos_user_thread_return` in `arch_context_init` in
  `arch/x86/x86_64/arch_x86_64.cc`. The other kernel uses of app symbols, including the
  image bounds and app data read through `aspace_image_alias`, need their physical or kernel
  view until conversion. `tests/static/check_x86_64_app_split.py`, the x86 twin of
  `tests/static/check_riscv_kernel_apphalf.sh`, enumerates the instruction and data references
  of every input the image kept and requires each kernel-to-app reference to be named, with its
  treatment, in `tests/static/x86_64_apphalf_allowlist.txt`. This catches a new site rather than
  assuming the four kernel sources in section 1.1 are the whole set.

App code reaches its own code and data by PC-relative offsets that the alias preserves; the
existing no-GOT guard (`tools/check-x86_64-no-got.sh`) remains. A link check refuses an
app-side instruction relocation into kernel text or data, and another refuses an app-side
`DIR64` word targeting the kernel. The retained-fixup check accounts for every app-side
absolute word, including app-to-app pointers. The image alone cannot show that a word is
missing, so the check's source is the input objects' own absolute relocations
(`R_X86_64_64` today), matched by their final addresses against the image's `DIR64` records.
The match covers the kernel's half too, since firmware's own pass moves exactly those records
when it loads the image elsewhere: `ld -m i386pep` writes no record for a weak reference to a
weak definition, which is how the kernel's pointer to the app's build stamp sat at its link
address until `user/include/kickos/app.h` stopped declaring the reference weak. The scan includes only allocated input sections that the final link kept;
debug relocations and discarded sections have no image word to match. A relocation whose target symbol is absolute
produces no record, its value being the same in every space, so the matcher expects none for
it. An unfamiliar absolute relocation type, any other live object relocation with no `DIR64`
record, or a record with no live object relocation is refused. A zero-fixup app passes only when both sides are empty.

The frame behind an app address is that address minus U. The x86 arch hook supplies U to the
portable seed and image-alias paths at runtime: the linker states the app's loader bounds and
its ordinary load delta, then `kernel/mem/aspace.cc` adds U to those bounds for task mappings
and resolves a loader frame as `app_va + load_delta - U`. Armv8a and rv64 continue to answer U=0.
No linker symbol claims a negative U before `aspace_init` has measured it. The pass runs
before the first seed, which is all the data path needs: root's space maps the image's own
data pages, later spaces copy root's live data while root lives and the snapshot
`data_template_fill` takes at root's release after that (`aspace_image_seed`), so every copy
inherits pointers already converted.

### 1.4 The frame pool is the arena

The frame pool is the UEFI arena `pick_arena` chooses in `arch/x86/x86_64/entry_x86_64.cc`,
published at boot by `ram_publish` in `arch/x86/chip/q35/chip_q35.cc`. The arena is split once,
at `frame_pool_init`, after `kickos::kmain` has taken idle's stack from it: what
`arch_ram_alloc` has handed out stays the kernel's, the rest is the pool, and `arch_ram_alloc`
refuses from then on so the two cannot overlap. The pool's bounds reach `frame_pool_init`
through a seam whose default reads the linker pair, as every other translating chip does. Its
size is whatever the firmware leaves, with no figure of its own.

### 1.5 The kernel half is supervisor-only by deletion

The firmware leaves every entry on the walk to the image and to conventional memory with the
user bit clear (`arch/x86/x86_64/ring3_x86_64.cc` says so, and its census measured it). So
**`ring3_init`'s grant is deleted**, and nothing replaces it: the kernel half is
supervisor-only because nobody opens it, and each task reaches only what its own space maps in
the user half. `ring3_init` keeps the fast-syscall registers and the per-core block.

- The census that measured the grant inverts: it must find no translation table and no kernel
  leaf a ring-3 access can reach.
- Supervisor-mode execution and access prevention stay refused by `ring3_init`: the bring-up
  images still open image pages to ring 3 with `ring3_grant_range` and run them at ring 0, and
  OVMF enables neither. Lifting the refusal for the kernel image and turning either on is a
  commit of its own with its own arm, not part of the enforcement.

### 1.6 Multicore

Nothing new. The kernel slots are shared by every root, so a kernel-half edit is seen by every
space; the editor already shoots an unmap down on the cores that hold the root; and the AP
trampoline page is kernel half. The two-core preset runs every arm below.

### 1.7 What it costs

- **Switch.** `arch_aspace_activate` writes CR3 when the incoming thread's space differs from the
  outgoing one's. PCIDs are off (`TAG_BITS_RECORDED`) and the firmware's leaves are not global,
  so each such write drops every translation, the kernel half's included. This is the price
  every x86 kernel with address spaces pays, and it is recorded rather than argued. If the
  figure calls for it, the first lever is marking the kernel half global, then PCIDs, each its
  own measured change.
- **Spawn.** A new space is a root table with the kernel slots copied, the app text mapped and
  the app data copied (`aspace_image_seed`), so a spawn costs in proportion to the app's data,
  as it does on armv8a.
- **Memory.** The pool comes out of the arena. The per-task tables come out of the pool, and
  `KICKOS_MAX_DOMAINS` defaults to 20 on a translating chip.
- **How it is measured.** Today's bench workloads never switch space: the semaphore ping-pong
  and the call/reply pair in `user/apps/common/bench/main.cc` are threads of root's own task.
  So the bench gains a cross-task ping-pong, a cross-task call/reply and a spawn/exit round trip,
  and all three run on the x86 QEMU bench preset on master before the change and on the branch
  after. An emulator counts work rather than cycles, which is what the figure is read as.

### 1.8 What proves it

- **The family's own arms**, `user/apps/common/selftest/selftest_aspace.cc`, build and run on
  `qemu-x86_64` for the first time, which is what makes that board a witness of the family.
- **The gates the other translating boards already carry extend to this one**:
  `tests/integration/gates/kernelhalf.cmake` (a ring-3 reach into the kernel half ends the task
  with `KOS_EXIT_FAULT`), `aspacefault.cmake` and `aspaceufault.cmake` (the user fault paths),
  and `appdata_no_kernel`. Each is registered today per arch, so each gains an x86 arm with the
  fault reporter's own banner.
- **New arms for what is x86's own**, both portable and run on every translating board.
  `kernel_state_unreachable` reads, from ring 3, the caller's own translation root and the
  calling core's per-core block (`KOS_ASPACE_OP_KERNEL_STATE`, the latter through
  `arch_cpu_block_addr`, which only x86 answers), and each read must end the task.
  `app_pointers_relocated` is the control that must pass: an app constructor ran, and
  initialized pointers to app text and to app data equal the addresses the running code
  computes, and are followed. The other controls are arms that already ran: every arm calls
  through the app window at its alias, every thread returns through
  `kickos_user_thread_return`, and `aspace_two_spaces_same_grant` and `process_ipc_same_addr`
  reach a delegated block at the same address in two tasks.
- **Both load cases.** The boot prints `x86_64 app relocation: base= load_delta= app_words=`,
  the delta measured against `__kickos_link_image_base`, an absolute script symbol: firmware
  rewrites the loaded header's `ImageBase` to the address it chose, so the header cannot say.
  OVMF honours the preferred 0x400000, so every image boots at delta 0 with firmware's pass
  idle. `selftest_rebased.efi` is the self-test linked at 1 TiB, which firmware cannot place,
  and `tests/integration/check_x86_64_rebased.sh` counts its run only when the boot reports a
  nonzero delta and a nonzero app word count, then judges its stream as the self-test's.
- **The static gates**: `x86_64_app_split` and `x86_64_app_split_controls`
  (`tests/static/check_x86_64_app_split.py` over every x86 image in the tree), which refuse an
  app relocation into the kernel, a narrow or unknown absolute type in the app window, a
  missing or orphan `DIR64` record anywhere in the image, an unlisted or stale kernel-to-app
  reference, and a re-based script symbol; and `appdata_no_kernel`.
- **Mutations**, each run on the one-core x86 tree (`.session` scratch, recorded in the
  M10.1.1 archive): putting back the image grant lets `kernelhalf` read the kernel's word; a
  user offset of zero refuses the boot in `app_relocate`; leaving the app words unmoved kills
  root at its constructor table, the first stored app pointer it follows; skipping the whole
  pass leaves `user_alias_offset` at zero, so the kernel faults writing root's first user
  return slot; moving `arch_syscall` back into kernel text is refused by
  `x86_64_app_split` and faults the first syscall; and dropping U at `arch_context_init`
  reddens the self-test at the first unprivileged thread that returns from its entry.
- **Gates whose expectation moves** with `KICKOS_MEMORY_ENFORCED` becoming 1 on this board --
  `rootfault`, `rootgone`, the trap red-zone classes -- are updated to what an enforcing board
  asserts, and the bring-up witness that asserted the broad grant asserts its absence.

### 1.9 Left out

PCIDs, a global kernel half and randomized placement, each a measured decision for later; the
port grant (M10.1.8); device windows at kernel-chosen addresses (M10.1.7).

### 1.10 For review

- **One offset, or a pair of translations.** A single constant U is the simplest thing that
  works, and it forbids placing a frame anywhere but at its physical address plus U. M10.1.7
  will want the kernel to choose; one offset now keeps that change in one place.
- **The alias, against moving the kernel instead.** Running the kernel from a high alias would
  free the low half and give x86 armv8a's layout with no U at all. It needs the same second
  relocation pass, for the kernel's fixups instead of the app's, plus a jump to the alias with
  every boot-time pointer converted on the way. The alias keeps the kernel where the firmware
  put it, so it is proposed.
- **Whether the bring-up images X1 to X6 stay.** The proposal keeps all six, changing only the
  one that asserted the broad grant.

## 2. The task-creation authority and a 32-bit authority word (M10.1.3)

**Creating a task becomes an authority, and the word that carries authorities stops being a
byte.** Today `KOS_SYS_TASK_CREATE` is gated by nothing: any thread mints tasks, the hole the
object-budget item has recorded since M8.5, where one unprivileged caller seats a thread in every
task slot and empties the pools. And the authority word is eight bits wide in
`kos_thread_params::authority`, `Thread::authority` and `kickos::CapAuthority`, with six spent.

- **The word is 32 bits wherever it appears**: `kos_thread_params::authority`,
  `Thread::authority`, `CapAuthority` and its mirror `kos_cap_authority` in
  `user/include/kickos/sys/abi.h`, the mask `kos_cap_narrow(KOS_CAP_AUTHORITY, mask)` takes, and
  `kickos_app_authority()` with `KICKOS_APP_AUTHORITY` in `system/include/kickos/sys/init.h`.
  In `kos_thread_params` it moves beside `core_mask`, the other 32-bit word, and `cap_count`
  moves into the byte behind `privileged`, so the struct keeps its size. Left behind the two
  words, `cap_count` cost three bytes of padding, which the spawn stager's copy of the struct
  turned into eight bytes of syscall frame: 452 bytes against the Due's 448-byte SVC reservation
  in CI, fixed in M10.1.4.
- **The tasks authority is bit 6** (section 8 lists every proposed name). It gates both ways a
  task is born:
  `KOS_SYS_TASK_CREATE`, and the implicit task `task_for` builds inside `thread_create_call`
  (`kernel/syscall/syscall_thread.cc`) when a spawn brings its own `mem_base` or changes
  privilege. Gating the explicit call alone would leave the M8.5 hole open through the spawn.
- **What stays ungated.** A thread spawned into the caller's own task needs no authority: the
  task's budgets already bound it. Seating a thread in a task the caller created stays gated by
  creatorship, as today, since only a holder of the bit can have created one.
- **Root holds `CAP_AUTH_ALL`**, which gains the bit, and the fallback app authority gains it
  too, matching the default composition's `memory`, `system` and `tasks` for `main`.
- **A bit and not a capability**, which the TODO item asked to settle: every other system power
  is a bit of this word, seated at spawn and narrowed by `kos_cap_narrow`, and a capability
  would need an object kind and a table slot to say the same thing.

**Arms.** A task create without the bit is refused `-KOS_EPERM` and succeeds with it; an implicit
task without the bit is refused, and a plain spawn into the caller's task still succeeds; the
first undefined bit, now bit 7, and the word's top bit are refused at spawn, which seats the word
as given; a narrow by the full 32-bit word keeps every held bit, since `kos_cap_narrow` intersects
and cannot grant, so an undefined bit in its mask is accepted and changes nothing; and the M8.5
adversary, an unprivileged thread minting tasks, refused at the first.

**Backends and cost.** Portable kernel, the ABI header, the init library and every app that
declares an authority. Nothing on the switch path. `Thread::authority` moves beside
`spawner_tag`: on a 64-bit target it takes the padding before `task` and costs nothing; a
32-bit TCB has no hole a word wide, so it costs the word and the tail back to `uint64_t`'s
alignment, 272 to 280 bytes on armv7m, and on an AMP node it shares that step with
`far_hold`. Measured across 23 presets, and stated in `thread_scalar_bytes`.

**What the part taught.** Seven arms had a worker create a task without the authority --
`prio_ceiling_narrow_only`'s member, rootgone's survivor, the donor of
`task_handoff_donor_exits`, the members of `grant_wider_refused` and
`grant_inherited_by_child_task`, and those of `spawn_refusal_frees_task` and
`spawn_refusal_frees_donor`, whose spawns must still be refused further down -- and now hand it
the bit; and
trapnest holds it so its deep spawn is still refused in `grant_region_admissible` rather than at
the new gate before it, which would have shortened the syscall descent it measures.

## 3. Handing out receiving, and the errno split (M10.1.4)

**An endpoint whose server is being restarted answers "try again", and one that will never be
served again answers "refused".** Today one number says both. `Endpoint::recv_holders` counts
the WAIT-bearing capabilities, and when it is zero a send or call answers `-KOS_EPIPE` at the
early-outs in `endpoint_send` and `endpoint_call` (`kernel/syscall/syscall_ipc.cc`), and parked
senders are woken with it when the last WAIT holder closes (`kernel/syscall/cap.cc`).
`endpoint_create` gives its creator a WAIT-bearing capability counted as a receiver, and
`kos_cap_narrow` narrows the authority pseudo-handle only, so an init that created an endpoint
cannot keep it open without being counted as its server.

- **A fourth right, the handout right**: its holder may delegate WAIT without holding WAIT
  itself. It is the only right a delegation may turn into another, and only into WAIT. It fits
  the entry: three rights bits plus the four of the call sequence's high half leave one spare bit
  in `CapEntry`'s byte, and the staged grant byte packs four type bits beside four rights bits.
- **`Endpoint::handout_holders`** counts the capabilities carrying it, a byte beside
  `recv_holders` in the padding ahead of `server` that `EP_NARROW_BYTES` counts: three bytes
  become four, five on an AMP node become six, and each stays inside one pointer alignment step,
  so no `Endpoint` grows. `endpoint_create` gives the creator all four rights, counted in both.
- **`kos_cap_narrow` narrows any capability**, not only the authority word; that is how an init
  drops WAIT and keeps HANDOUT, and a narrow that drops the last WAIT runs the same wake a close
  does.
- **The split.** With no local receiver, a send or call answers `-KOS_EAGAIN` while a handout
  holder exists and `-KOS_ECONNREFUSED`, 111 and new, once none does, and parks in neither case.
  A sender parked when the last receiver leaves is woken with the same choice. `-KOS_EPIPE` keeps
  exactly one meaning, the server died holding the request, from the reply arm in `cap.cc`. The
  call path's refusal of a dying receiver before it took the request is the no-receiver case and
  joins the split. The fast path (`kernel/syscall/syscall_ipc_fast.cc`) answers no refusal of
  its own and falls through to `endpoint_call`, so it changes by nothing.
- **Every caller testing for `-KOS_EPIPE`** moves with it: `tests/tap/tap.cc`,
  `user/src/newlib_stubs.cc`, `user/src/driver_service.cc`, `user/include/kickos/sys/emit.h` and
  `driver_service.h`, the SPI and I2C driver headers, the `xmcssc` and `k64dspi` drivers,
  `system/init/sim/service_list.cc`, `lib/strerror.cc`, the apps and unit tests that assert it,
  and `tests/integration/check_sim_drvdeath.sh`. Far AMP endpoints keep today's codes: the wire
  carries no errno and a far node has no local receiver to count.

**Arms.** No receiver and no holder answering `-KOS_ECONNREFUSED`, the existing endpoint-death
arms updated to it; a holder present answering `-KOS_EAGAIN`, then WAIT delegated at a spawn and
a call that succeeds; a holder that cannot receive and is not counted as a receiver; a caller
parked when the last receiver dies, with and without a holder; the server dying with the request,
still `-KOS_EPIPE`; the same refusals through the register call path.

**Backends and cost.** Portable IPC code only; no arch assembly reads `recv_holders`. The extra
test sits on the refusal branch, and `static_assert`s hold `CapEntry` and `Endpoint` at their size.

**What the part taught.** The console clients (`emit.h`, `tap.cc`, `newlib_stubs.cc`) close
their capability on `-KOS_ECONNREFUSED` only: a driver an init may restart answers
`-KOS_EAGAIN`, and closing on it would throw away a route that comes back. The handover probe
expects `-KOS_ECONNREFUSED`, the init's own capability and its handout right going in the same
close. The arms are `endpoint_handout`, which walks the whole restart shape on one endpoint, and
`endpoint_handout_parked`, a parked sender released by a narrow; removing the delegation
exception, the narrow's accounting, or the `-KOS_EAGAIN` answer each turns one red.

## 4. Deaths and first receives, reported to the creator (M10.1.5)

**A creator learns, on a notification it names, when a task it created has died and when it has
become ready.** Today `sched::exit_current` wakes joiners, task slayers and `kos_wait_last`, and
raises no notification; `notify_raise` has two callers, `notify_signal` and `irq_event_isr`; and
nothing records a thread's first receive. The kernel does not know which endpoint a task serves,
so the ready report has to be told.

- **`kos_task_watch(task, notify_cap, ready_ep)`**, a new call, arms both reports on a task the
  caller created (`-KOS_EPERM` otherwise, through `task_created_by`). A capability handle means
  something only in its holder's table, so the task records the OBJECTS: the notification by
  generational object handle, with the capability's badge as the bits to raise, and the
  endpoint whose first receive means ready, or none for a task that serves nothing.
- **The watch names the notification, and holds no reference.** It resolves the object by
  generational handle at each raise, so a notification freed and its slot seated again is never
  raised into. Whoever can receive the report holds a capability or a binding, and either keeps
  the object alive, so the creator closing its own capability changes nothing for them; a
  report nobody can receive goes nowhere. M10.1.5 built the reference first and removed it: no
  arm could observe it, and it cost a refusal. The watch ends when the watched task's slot is
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
- **`kos_task_state(task)`**, a new creator-only call, answers whether that instance has members
  and whether it has become ready, and `-KOS_EBADF` once its slot is freed. A notification's bits
  merge a death and a ready into one wake, so the init reads the state of each task the badge
  names, deaths first, as `design-m10-composition.md` requires.
- **The generation is the task handle's.** A restart is a new kernel task and a new handle, whose
  generation (`Task::gen`) differs from the dead one's, so the instance a ready event belongs to
  is the handle the init holds, and no counter is added.
- **Nothing names the init.** The report goes to whoever armed it, which only the creator can,
  so a nested init's children report to it.

**Arms.** Death of a task that exits and of one that faults; a respawn from the death wake,
after the hold is released, taking the same window and the freed slot back; the creator closing
its notification capability before the death, the report still raised through a second name
and a binding; ready raised once, and only for the armed endpoint; a stale handle
answering `-KOS_EBADF` after a restart while the new one waits for its own first receive; arming
a task one did not create refused; a second-level creator receiving its own children's reports.

**Backends and cost.** Portable kernel. Two handles, the badge's bit index and a ready byte on
`Task` for each of `KICKOS_MAX_TASKS`, 20 bytes a task on a 32-bit target where it was 12; one
call and compare on the receive slow path and one test on the last exit. The bit is an index
and not a mask because a mask cost four more bytes a task, which the ESP32-C6 AMP node, whose
arena backs its default stacks almost to the byte, could not spare. That image runs its code
from the SRAM its arena comes out of, and the pool is aligned to the 8 KiB TLS stride, so its
`selftest_p3` had no byte to spare at M10.1.4 and has 176 at M10.1.5, the arms folded into one.

**What the part taught.** The arm is `task_watch_reports`: a worker holding the task authority,
above its member, walks one instance from arming to release -- a wait on another endpoint staying
quiet, the ready report once, the death after the teardown, `-KOS_EBADF` once released;
`task_creator_gate` adds the stranger's refusal of both calls. Removing the death report or the
receive hook turns it red, and so does a ready report that wakes the creator at once: on armv7m
the switch is deferred to PendSV, so the scheduler's current thread moves to the creator while
the receiver is still parking, and the receive parks the creator instead. The two syscalls join the
telemetry name tables (`tests/unit/telemetry/gen_idmap.cc`, `tools/kicktrace.py`).

## 5. A window list in spawn (M10.1.6)

**A spawn carries a list of windows: device registers, shared memory read-only or read-write,
and port ranges.** Today it carries one device window, `mmio_base` and `mmio_size`, recorded as
`Thread::dev_base` and `dev_size`, checked for a single holder by `dev_window_free`
(`kernel/thread/thread.cc`) and read by `caller_holds_mmio_block` and `caller_holds_mmio_reg`
(`kernel/syscall/syscall_mem.cc`), which gate `kos_periph_enable` and `kos_periph_reg_write`. A
shared region can reach a task only as its data region, `mem_base`, always read-write.

- **The entry**: `struct kos_window { uintptr_t base; uint32_t size; uint8_t kind; uint8_t
  flags; }`, the kind a device, memory or port window, the flags read-only and uncached.
  `kos_thread_params` loses `mmio_base` and
  `mmio_size` and gains `windows` and a 16-bit `window_count`. `mem_base` stays: it is the task's
  own data region, whose presence is what builds an implicit task.
- **The bound is the kernel's, not the ABI's.** A thread keeps at most
  `KICKOS_MAX_THREAD_WINDOWS` entries, whose default is what the protection unit
  has left after the regions every thread takes: four of `KICKOS_MPU_MAX_REGIONS`' eight go to
  code, static data, the task's data region and the stack, so the default and the ceiling are
  four; the M10.2 manifest exports it and admission checks it offline. A longer list is refused
  `-KOS_ENOMEM`, the code for descriptor capacity.
- **Possession is per thread**, as the ruling states and as a region board enforces it. On a
  translating board the mapping is in the task's space, so a sibling thread reaches a window
  while its holder lives without being its holder; the kernel's checks -- one holder, the
  peripheral seams -- still ask the thread. The record is the thread's own region list
  (`Thread::mpu`), whose device entries are exactly its device windows, a self-grant never
  carrying the device type, so `Thread::dev_base` and `dev_size` go and no array is added.
- **A window outlives no holder.** Its mapping is made when its holder is created and removed
  when that thread exits, not when the task dies, and on a multicore board the unmap completes
  its shootdown before the thread's record releases the window. The one-holder check counts a
  window until that release: today `dev_window_free` skips a thread already marked dying, which
  would let a second task be granted a device the dying holder, or its surviving siblings through
  the task's mapping, can still reach. So a window is granted again only once no space maps it
  and no thread holds it. The release is `exit_current` dropping the holder's device regions
  where the dying mark used to be, before its teardown can wake a supervisor; section 6's unmap
  goes in front of it.
- **The checks.** A device window stays exclusive, per entry and within one list, and takes no
  flag; a memory window must lie in a block the spawner reserved, as `mem_base` must today, under
  the self-grant's admission, and the read-only flag encodes it read-only; the peripheral seams
  accept any device window the caller holds. The ports kind is refused `-KOS_ENOTSUP` until
  section 7 lands it, so that part adds no second ABI change, and a translating board refuses a
  memory window the same way until section 6 chooses where it sits.
- **The list is staged in the kernel instance**, `Kernel::window_stage`, and not on the spawner's
  stack as the grant list is: the stack copy moved the armv7m SVC and rv32 privileged-syscall
  reservations a whole step, and the spawn holds the kernel lock from the copy to the commit.

**Arms.** Two windows, both reachable and both accepted by the peripheral seams; overlap with
another thread's window and within one list; a list over the knob; a read-only region that reads
and faults on a write; `dev_window_exclusive` and `mmio_grant` moved onto the list. The sim grants
exactly one window today (`arch_mpu_region_encodable` in `arch/sim/sim.cc`) and gains a second.

**Backends and cost.** Portable kernel, every region backend's encoding (armv6m, armv7m, armv8m,
the rv32 PMP, rxv3), the sim, and every spawn caller under `system/init/` and in
`user/include/kickos/kos.h`. A window array in every thread control block was to be the part's
main cost, and the one most likely to be too dear on the smallest boards; it was not needed.

**What the part taught.** The array never came: the region list already was the record, so a
thread control block is 8 bytes smaller on a 32-bit target (272 on armv7m) and 16 on a 64-bit one,
and the instance carries the staged list instead, 48 bytes on a 32-bit target. The trap red zones
are unchanged, `window_admit` and the window loop of `thread_create` being out of line so their
locals stay off the SVC chain's widest frames. Only PMSAv7 ignored the write bit, every data
descriptor having been read-write until now, so armv6m and armv7m gained the read-only encoding
(`MPU_RASR_AP_URO`); PMSAv8, the rv32 PMP, rxv3 and the K64 SYSMPU already honoured it. The sim
admits either half of its register span. The arms are `mmio_grant`, which adds the list's own
refusals; `dev_window_exclusive`, which adds a list naming one window twice and a two-window
holder whose second entry is then held; `window_list` on the sim, the seam through both windows
and a respawn from the death wake of a server holding one; and `window_memory_ro`, where a child
in a task of its own reads through a read-only window and dies on its write, a read-write window
reading back what that write left. A thread may also hold one block through a read-write and a
read-only window, and the kernel's pointer checks answered from the read-write one, so a
syscall wrote what the thread could not (review): `user_range_ok` and
`user_readable_and_writable_ok` now take no write from the region set wherever a read-only region
overlaps it, whichever region a region backend obeys, and the same arm has such a child name the
block as a reply buffer (`tests/unit/rangecheck` pins the rule on both checks). A translating
backend's rights are its mapping's, which section 6 gives each window. Removing the check within a list, the read-only encoding or the
seam's walk past the first window turns them red. Removing the release does not: on a single-core
board the woken supervisor runs only once the holder has exited, so the release is load-bearing
where it runs on another core during the teardown, and no QEMU preset is a multicore region board.
The ESP32-C6 AMP node image had 176 bytes left and this part is about 1 KiB of code, so the
partition's shared window went from 64 to 32 KiB and each node slice from 224 to 240 KiB, as the
RP2350's already are; the images use about 10 KiB of that window.

## 6. The kernel chooses where a window sits (M10.1.7)

**On a translating board the kernel maps each device and memory window at an address it chooses
in the task's space, and `kos_window_addr` asks it.** Today no translating board maps a device
window at all: `arch_mpu_region_encodable` answers false in `arch/arm64/armv8a/arch_armv8a.cc`,
`arch/riscv/rv64imac/arch_rv64imac.cc` and `arch/x86/x86_64/arch_x86_64.cc`, so a spawn carrying
a window there is refused. This part is the first device mapping in a user address space, not
a move of one.

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
- **`kos_window_addr`** calls a new window-address syscall with a physical base, which answers
  the caller's task
  address for the window it holds at that physical base, and `-KOS_EPERM` for one it does not.
  On a region board and on the sim it answers the physical base, the only address there is.

**Arms.** On each translating preset, the address answered differs from the physical one and the
device reads through it (the PL031 on `virt`); a window the caller does not hold is refused; the
mapping is gone when its holder exits while a sibling survives, the sibling faulting on it, and
a second task is refused the device until then; the mapping is made again for the next instance; a region board answers the
physical base.

**Backends and cost.** Portable kernel, armv8a, rv64 and x86_64. A page map per window at spawn,
an unmap and an invalidation at the holder's exit, and a mapping record per window in the task's
ranges.

**What the part taught.** The record is a `VR_WINDOW` range naming its holder, a thread slot, and
for a memory window the donor domain it holds a reference on; both fit the padding of
`VirtualRange`, so neither the list nor the thread control block grows. The window area is 1 GiB
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
refused an address for a device it does not hold and a window outside every aperture, a second
task refused the device while it lives, its sibling faulting on the address once it has exited,
and the next instance mapping it again; `window_memory_ro` now runs on the translating presets
too, its children reaching the block where `kos_window_addr` says and reporting over an endpoint,
a task's data being its own there; and `window_list` on the sim, which answers the physical base.
Removing the unmap at a holder's exit or answering the physical base turns them red. Review
found three holes, now closed: a block mapped cacheable could be lent to a window uncached, so a
memory window, a self-grant and a task's data handoff now take no memory type another live
mapping of the same frames does not carry (`aspace_frames_type_ok`, `memory_type_free`,
`-KOS_EBUSY`), a window asking again where it is mapped, after the spawn's own data, and a
region board asking the held task domains beside the live threads, an empty task holding its data
with no member; a list could name one
block in two windows and `kos_window_addr` could answer only the first, so a list names a block
once; and a region board answered for its stack or data region as for a window, so the region set
marks its windows. `window_memory_ro` witnesses all three. The spawn
chain's frame grew under g++ 13, so the x86 reservations rose to it, at one core and above: SYSK
to 1800 and 2296, SYSPRIV and SYSPRIVSW to 1856 and 2304, and the spawn floor to 2432 and 3136.

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
record lands with this part.

## 8. Every header the ABI changes touch

The proposed names, by part. They are a listing and not the tree: each part may rename what its
code teaches it to.

```c
/* user/include/kickos/sys/abi.h */
KOS_AUTH_TASKS = 1 << 6,                 /* 2: kos_cap_authority, mirrored by AUTH_TASKS */
uint32_t authority;                      /* 2: kos_thread_params, beside core_mask */
KOS_CAP_HANDOUT = 1 << 3,                /* 3: kos_cap_rights */
KOS_SYS_TASK_WATCH,                      /* 4: (task, notify_cap, ready_ep) -> 0, or -KOS_E* */
KOS_SYS_TASK_STATE,                      /* 4: (task) -> KOS_TASK_LIVE | KOS_TASK_READY, or -KOS_EBADF */
struct kos_window {                      /* 5 */
    uintptr_t base; uint32_t size;
    uint8_t kind;                        /* KOS_WINDOW_DEVICE, KOS_WINDOW_MEMORY, KOS_WINDOW_PORTS */
    uint8_t flags;                       /* KOS_WINDOW_RO, KOS_WINDOW_UNCACHED */
};
struct kos_window const* windows;        /* 5: kos_thread_params, replacing mmio_base, mmio_size */
uint16_t window_count;
KOS_SYS_WINDOW_ADDR,                     /* 6: (base) -> the caller's address for it, or -KOS_EPERM */
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
| | 6 | the window-address syscall |
| | 7 | the port-write syscall |
| `system/include/kickos/sys/errno.h` | 3 | the refused errno, 111; `KOS_EPIPE` and `KOS_EAGAIN` reworded |
| `user/include/kickos/sys.h` | 3 to 7 | the wrappers `kos_task_watch`, `kos_task_state`, `kos_window_addr`, `kos_port_reg_write`; the `kos_ram_alloc` comment |
| `user/include/kickos/kos.h` | 2, 5 | `kos::create`'s authority and window arguments |
| `system/include/kickos/sys/init.h` | 2 | `kickos_app_authority` and `KICKOS_APP_AUTHORITY` 32 bits |
| `user/include/kickos/sys/driver_service.h`, `emit.h`, `driver/spi.h`, `driver/i2c.h` | 3 | the no-receiver answers |
| `user/include/kickos/sys/abi_probe.h` | 3 to 7 | probe selectors the new arms need, selftest only |

`arch/include/kickos/arch/arch.h` changes too -- the window area of section 6, the port seams of
section 7 -- and is the kernel's internal interface, not the ABI.

## 9. The baseline

M10.1.1's bench rows -- a cross-task ping-pong, a cross-task call/reply and a spawn/exit round trip
for a thread and for a task -- are the instrument. The baseline is every QEMU bench preset on the
tree M10.1.2 closes on, archived with its captures in `docs/archive/M10.1.2_baseline.md`, and each
part re-measures the presets its backends run, beside the static figures it can move:
`KICKOS_THREAD_EXPECTED_SIZE`, `sizeof(Task)`, the trap red-zone reservations, and `hello`'s
`.text` and `.bss` on the smallest boards. The x86 figures count emulated work, as section 1.7
says, and silicon figures come from `tools/bench/bench.sh` on the boards `bench-present.sh`
answers present.

## 10. For review

- **Per-thread windows on a translating board.** The ruling keeps possession per thread, and the
  mapping is in the task's space, so a sibling reaches what it does not hold while the holder
  lives. Recording windows per task instead would match the translating boards and bound the
  array by `KICKOS_MAX_TASKS` rather than by threads, but a region board programs its unit per
  thread.
- **One call for the reports, or two.** `kos_task_watch` arms death and ready together because
  the init always wants both; a watcher that wants deaths only passes no endpoint.
- **`mem_base` outside the list.** It stays because it is what builds an implicit task; folding
  it in would make one kind of memory window mean "this is my task's data" and need a flag to say
  so.
