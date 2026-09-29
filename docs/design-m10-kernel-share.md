<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M10.1 -- the kernel share

> **Status: ACTIVE -- written one section per part, each reviewed before its code.** This
> revision carries section 1 only, the x86 address spaces M10.1.1 lands. The sections for the
> ABI changes, and the table listing every header they touch, come with M10.1.2.
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
