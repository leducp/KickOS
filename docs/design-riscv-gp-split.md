<!-- SPDX-License-Identifier: CECILL-C -->
# Design: RISC-V gp small-data kernel/app split -- full-C++-under-PMP feasibility

> **Status: LANDED** -- the split shipped and is CI-gated: `riscv_no_smalldata`,
> `qemu_riscv_cxxtest` and `qemu_riscv_mpu_fault` run in the `qemu-riscv-mpu` job, so a U-mode
> throw under PMP is a green gate rather than a hope. `user/` objects build
> `-msmall-data-limit=0` and app globals land in `.appdata`/`.appbss`. The option analysis below
> is kept as the why. See `design/README.md` for the marker taxonomy.

Decision record for one question: where the RISC-V `gp` small-data window goes when full C++
(exceptions/STL/RTTI) must run under PMP. Full C++ under memory protection already worked on ARM
(EHABI plus PMSA/SYSMPU) and RX (SjLj). On esp32c6 `rv32imac` and qemu `virt` it did not, because
the prebuilt libstdc++/libsupc++/libgcc put their writable globals in the `gp` window
(`.sdata`/`.sbss`, addressed through the single `gp` register relative to `__global_pointer$`),
which linked KERNEL-side, outside the app grant, so a U-mode throw or catch faulted under PMP.

Shipped contract: `reference/porting.md` (the `gp` anchor for full-C++ under MPU). Teaching:
`book/exceptions-and-rtti-under-memory-protection.md`. Confirmed on C6 silicon (2026-07-19), the
U-mode cxxtest passing with the anchor inside `.appdata`, per `m2-readiness.md`.

## Verdict

- A **gp kernel/app SPLIT via two `gp` bases (per-domain gp, reload on the kernel<->user
  boundary) is INFEASIBLE** for KickOS's single-image link, and buys nothing even where
  it is possible. Reason (proven below): there is exactly one `__global_pointer$` linker
  symbol; every `gp`-relative immediate is computed against it at link time, and linker
  gp-relaxation assumes a single fixed anchor. Two runtime `gp` values make one of the two
  reference sets address the wrong memory. It cannot separate kernel small-data from app
  small-data in one ELF.
- The best option is **NOT a split at all**: **empty the kernel side of the ONE gp window**
  (compile the KickOS libs `-msmall-data-limit=0` so they emit no small-data) and anchor
  `__global_pointer$` INSIDE the app grant, so the single window holds only app + C++-runtime
  small-data and can be granted to U-mode. This is the cheapest real fix and needs NO
  toolchain rebuild. It fixes esp32c6 AND `virt` together (same `rv32imac` backend, same
  linker-script pattern).
- **PMP cost: +0 entries** in the recommended variant (the gp window folds into the existing
  `.appdata` NAPOT region). **Context-switch delta: ~2 instrs**, because `gp` is a single
  link-time constant but now anchors LIVE app small-data, so a U-mode thread can write its own
  `gp` and `switch.S`'s restore epilogue (`.Lrestore`) force-reloads `gp` to `__global_pointer$`
  before returning to any (re)dispatched thread (self-healing per switch).

## The invariant that must hold

> No memory reachable by an unprivileged (U-mode) thread's PMP grant may contain kernel
> writable state, AND every `gp`-relative access the C++ runtime performs in U-mode must
> land inside that same grant.

The fault was a joint violation: the runtime's `gp`-relative writable globals sat in a window that
was kernel-side (so U-mode faulted reading them), yet the window also carried scheduler-critical
kernel state (so it could not simply be handed to U-mode). The fix must separate the two owners,
then grant only the app owner.

## Background: the gp window and why it faults

RISC-V GCC routes globals of size <= `-msmall-data-limit` (default 8 B) into `.sdata`/`.sbss`,
addressed as `gp + imm` where `imm = symbol - __global_pointer$`; `gp` is set once at boot
(`startup.S`, `.option norelax; la gp, __global_pointer$`). The psABI reach is
`__global_pointer$ +/- 0x800` (4 KiB total).

MEASURED on the linked esp32c6 cxxtest image before the fix, with the `.sdata`/`.sbss` catch-all
inside the kernel `.data`/`.bss` and `PROVIDE(__global_pointer$ = . + 0x800)`: `eh_globals`,
`seen_objects`/`unseen_objects` (the DWARF FDE registry) and `_impure_ptr` sat at ~0x40820000, just
below `__global_pointer$` = 0x40820800, kernel-side and outside the app window
[0x40828000,0x40830000). A U-mode throw read and wrote them, and faulted under PMP.

The app's OWN objects already dodged this: `kickos_add_application` passes `-msmall-data-limit=0`
to the app TUs, so their globals leave the gp window and land in the app-side `.appdata`/`.appbss`
catch-all. The PREBUILT vendor libs are not compiled that way.

## Evidence gathered (qemu-riscv, RISCStar rv32imac/ilp32 multilib, this tree)

1. **The KickOS libs DO emit gp small-data**, the crux of the "empty the kernel side" option.
   `objdump -h` on the built archives (KICKOS_HAVE_MPU=1):
   - `libkickos_arch_rv32imac.a`: `.sbss g_arch_current, g_arch_next, g_isr_depth, g_clint_msip`;
     `.sdata g_irq_masked, g_inject_line`, i.e. the scheduler's most sensitive state (the
     current/next context pointers and the CLINT msip doorbell) lives in the gp window.
   - `libkickos_kernel.a`: `.sbss g_kernel, g_default_user, g_console_panicking, g_led_on`.
   - `libkickos_chip_virt.a`: `.sdata SystemCoreClock`. `libkickos_lib.a`: none.
   So the window is NOT app-only, and granting it to U-mode without emptying it first would let an
   unprivileged thread overwrite `g_arch_current` or `g_clint_msip`. Total compromise. This is why
   a naive "grant the gp window app-side" is unsafe on its own.

2. **`-msmall-data-limit=0` empties the KickOS side.** Recompiling `arch_rv32imac.cc` with the flag
   moves `g_arch_current` to `.bss.g_arch_current` and `g_irq_masked` to `.data.g_irq_masked`, with
   zero `.sdata`/`.sbss` sections. Our sources, our build flags, no library rebuild.

3. **No newlib small-data is kernel-referenced.** A freestanding (non-full-C++) selftest image
   pulls in NONE of newlib's small-data (`_impure_ptr`, `__malloc_av_`, `__sf`, reent): those
   appear only once libstdc++/libsupc++ is linked, dragged in by the APP's `operator new` and
   exception machinery, all U-mode. The kernel archives' only newlib undefined is `memset` (pure
   `.text`, no small-data). So in a full-C++ image every gp small-data resident is either
   KickOS-owned and movable by (2), or C++-runtime/newlib and app-side U-mode-only. There is no
   third, kernel-shared class, which is what makes "empty + grant" sound.

4. **The runtime residents that must land app-side** (`nm` on the full-C++ image): `eh_globals`,
   `__new_handler`, `__terminate_handler`, `__unexpected_handler`,
   `seen_objects`/`unseen_objects`, `DW.ref.__gxx_personality_v0`, `_impure_ptr`; plus
   `__malloc_av_` and friends, already `.data`-sized (over the limit) and already falling through
   to `.appdata`.

## Option evaluation

### Option 1 -- Rebuild the RISC-V C++ runtime (and newlib) `-msmall-data-limit=0`

Recompile libstdc++/libsupc++/libgcc (and libc, since `_impure_ptr`/malloc small-data also ride
the window) so their writable globals land in ordinary `.data`/`.bss`, captured by the existing
app-side catch-all.
- Fixes both boards: yes (backend-shared). PMP entries: +0. Context-switch: +0.
- REJECTED as the primary fix, kept only as the theoretical root-cause baseline. Burden is HIGH and
  against the house rule: `-msmall-data-limit` is a per-TU compile flag baked into the prebuilt
  `.a`, so changing it means recompiling the pinned RISCStar multilib (gcc/newlib/libstdc++
  bootstrap) and breaking the "one pinned, reproducible prebuilt, no distro drift" guarantee the
  toolchain file states. Heavyweight and reproducibility-eroding, for an outcome Option 4 reaches
  with a linker-script plus build-flag edit and no rebuild.

### Option 2 -- Per-domain gp: two small-data windows, reload gp on the kernel<->user boundary

Give the kernel and the app SEPARATE gp windows and reload `gp` at the M<->U context boundary.
**Infeasible for a single-image link.** Reasoning, from the code plus the psABI:

- `gp` is NOT saved or restored per thread. It is a link-time constant set once in `_start`
  (`switch.S` header: "gp/tp are NOT saved"; the 128 B frame has no gp slot). A per-domain gp would
  need a gp slot in the frame AND a `la gp, <kernel_gp>` (norelax) at the very top of `trap_entry`,
  because the trap handler (M-mode kernel) begins executing with whatever gp the interrupted U-mode
  thread had. That part is mechanically possible.

- **The killer is the single `__global_pointer$`.** A `gp`-relative reference's immediate is
  `symbol - __global_pointer$`, computed at link time against the ONE `__global_pointer$` symbol,
  and linker gp-relaxation (`.option relax`, on by default) rewrites absolute `la`/loads into
  `gp`-relative forms assuming that one fixed anchor. In a single ELF you cannot have kernel refs
  resolved against a kernel_gp and app refs against an app_gp: the linker knows only one anchor, so
  kernel `.sdata` and app `.sdata` are BOTH addressed relative to it and must BOTH sit within its
  +/- 0x800 reach, i.e. back in ONE window. Reloading `gp` to a second runtime value makes one of
  the two precomputed-immediate sets address wild memory. Two anchors need two separate links (a
  distinct kernel ELF and app ELF); KickOS links the app against `libkickos*` into ONE image, so
  that escape hatch is unavailable.

- If instead you removed the kernel's gp dependency entirely (compile the kernel `norelax` plus
  `-msmall-data-limit=0` so it emits no `.sdata` and never reads `gp`), there is no kernel gp to
  preserve and nothing to reload, which is exactly Option 4 with one gp. So Option 2 either is
  impossible (two live anchors) or degenerates into Option 4 (one anchor, kernel emits none).

- PMP: even in the degenerate case an app-side gp window is +0/+1 entries, the same as Option 4,
  while the gp-reload machinery adds context-switch cost and a `trap_entry` hazard window for no
  gain.

REJECTED: no configuration of Option 2 beats Option 4, and the literal two-gp form does not link.

### Option 3 -- Compile the WHOLE system `-msmall-data-limit=0` (no gp small-data anywhere)

Passing the flag to every KickOS TU AND the app empties the KickOS side of the window, but the
PREBUILT vendor libs keep their `.sdata`/`.sbss` (their `.a` was compiled with the default limit).
Those residents (`eh_globals`, `_impure_ptr`, ...) still land in the gp catch-all wherever the
script puts it. So this alone does NOT relocate the runtime's small-data: moving THAT still means
rebuilding the libs (reduces to Option 1) or relocating the catch-all app-side (Option 4).

REJECTED, insufficient by itself. Also note that passing `-msmall-data-limit=0` to the APP is the
wrong move: the app's DWARF-EH refs (`DW.ref.*`, LSDA datarel) resolve `gp`-relative, so keeping
app small-data enabled is REQUIRED for unwinding (see `design-cxx-under-mpu.md`).

### Option 4 -- Empty the kernel side, anchor gp INSIDE the app grant (RECOMMENDED)

Force `-msmall-data-limit=0` on the KickOS libs ONLY (kernel/arch/chip/lib), so their small-data
moves to `.data`/`.bss` and the gp window holds NOTHING kernel-owned (evidence 1+2). Then move the
`.sdata`/`.sbss` CATCH-ALL from the kernel `.data`/`.bss` into `.appdata`/`.appbss`, and set
`__global_pointer$` inside that block. Now the single gp window contains only app + C++-runtime
small-data (evidence 3+4) and is covered by the app grant. Keep the app compiled WITH small-data
so unwinding still works.

- Fixes both boards: yes. `esp32c6.ld` and `virt.ld` share the layout, so the edit is identical.
- PMP entries: **+0** where the gp window folds into the existing `.appdata` NAPOT region. The
  runtime small-data is well under 0x800, so `__global_pointer$` at the block's middle reaches all
  of it and the whole thing fits inside the current 4 KiB `_appdata_size`. A separate gp-window PMP
  region would be +1 and is unnecessary.
- Context-switch: **~2 instrs**, one gp, one `__global_pointer$`, gp-relaxation intact. See step 5.
- Burden: a build-flag change on four KickOS libs plus a linker-script move, no toolchain rebuild.
  Cost is a small KickOS code-size increase (absolute rather than gp-relative addressing of kernel
  globals) and the loss of the ISA's small-data optimization for KickOS code, negligible for a
  microkernel.

This is the layout `design-cxx-under-mpu.md` calls "gp-in-appdata". This study's contribution is
the option-space proof that it is the RIGHT one (Options 1/2/3 are dominated or infeasible) and the
measured confirmation that step 1 is load-bearing.

## Recommended approach -- concrete steps

1. **cmake:** `-msmall-data-limit=0` on the four KickOS RISC-V libs (kernel, arch_rv32imac, chip,
   lib) under the same `rv32imac AND KICKOS_HAVE_MPU` guard that gates the app flag. NOT on the app.

2. **linker (`esp32c6.ld` + `virt.ld`, KICKOS_HAVE_MPU path):** move the small-data catch-alls out
   of the kernel `.data`/`.bss` into `.appdata`/`.appbss`, with `PROVIDE(__global_pointer$ = ...)`
   in the `.appdata` small-data sub-block. `arch_domain_static_regions`
   (`kernel/domain/domain.cc`) is UNCHANGED: the gp window is now inside the app-data region it
   already reports, so the PMP entry count does not move.

3. **guard (security-critical):** the flag must be COMPLETE. One KickOS global left in `.sdata` is
   now inside an app-granted region, a privilege-escalation vector. REJECTED: a targeted per-TU
   flag, because a future kernel global would silently re-populate the gp window. Keep it fleet-wide
   over all four libs AND gate it in CI: `nm`/`objdump` the KickOS `.a`s and assert zero
   `.sdata`/`.sbss` sections.

4. **ASSERT gp reach:** a linker `ASSERT` that the runtime small-data sub-block fits
   `__global_pointer$ +/- 0x800`, so a future runtime that grows its small-data fails loudly rather
   than mis-addressing.

5. **restore gp on dispatch (security-critical):** with `__global_pointer$` inside the live app
   grant, a malicious U-mode thread can set its own `gp` to any value, and the next thread would
   run its `gp`-relative small-data loads and stores at attacker-chosen addresses, defeating the
   per-thread-stack guarantee at U-level. `switch.S` does NOT save or restore gp per thread, so the
   restore epilogue `.Lrestore` (the choke point every dispatch path funnels through) forces
   `gp = __global_pointer$` under `.option norelax` before every `mret`. gp is one link-time
   constant for the whole image and the kernel uses no gp-relative code, so the reload is a safe
   ~2-instruction self-heal per switch.

## See also

- `design-cxx-under-mpu.md` -- the broader full-C++-under-MPU experiment; reaches the same
  "gp-in-appdata" layout and reports it proven on qemu-riscv. This doc is the focused option-space
  justification (rules out per-domain gp) and the measurement that the KickOS libs emit gp
  small-data today.
- `reference/porting.md` -- the `rv32imac` arch / PMP backend seam, and the shipped `gp`-anchor
  contract.
