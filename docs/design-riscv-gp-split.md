<!-- SPDX-License-Identifier: CECILL-C -->
# Design: RISC-V gp small-data kernel/app split -- full-C++-under-PMP feasibility

Scope: full C++ (exceptions/STL/RTTI) under memory protection works on ARM (EHABI +
PMSA/SYSMPU) and RX (SjLj). On RISC-V (esp32c6 `rv32imac` under PMP, and qemu `virt`)
it does NOT: the prebuilt libstdc++/libsupc++/libgcc put their writable globals in the
`gp` small-data window (`.sdata`/`.sbss`, addressed via the single `gp` register relative
to `__global_pointer$`), which today links KERNEL-SIDE, outside the app grant -- so a
U-mode throw/catch faults under PMP. This doc is a feasibility study of the option space
for splitting that window kernel-vs-app, with per-option cost/risk, and a recommendation.

FEASIBILITY STUDY, not landed. Sibling of `design-cxx-under-mpu.md` (the broader full-C++
experiment, which independently reached the same recommended layout and calls it "proven
on qemu-riscv"). This doc adds the rigorous per-option evaluation -- in particular it
rules OUT the "two-gp / per-domain gp reload" split -- and backs the recommendation with
the measured fact that the KickOS libs themselves emit gp small-data today.

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
  `.appdata` NAPOT region). **Context-switch delta: ~2 instrs** -- `gp` is a single link-time
  constant, but because it now anchors LIVE app small-data a U-mode thread can write its `gp`
  register, so `switch.S`'s restore epilogue (`.Lrestore`) force-reloads `gp` to
  `__global_pointer$` before returning to any (re)dispatched thread (self-healing per switch).

## The invariant that must hold

> No memory reachable by an unprivileged (U-mode) thread's PMP grant may contain kernel
> writable state, AND every `gp`-relative access the C++ runtime performs in U-mode must
> land inside that same grant.

The fault today is a joint violation: the runtime's `gp`-relative writable globals sit in a
window that is kernel-side (so U-mode faults reading them), yet the window also carries
scheduler-critical kernel state (so it cannot simply be handed to U-mode). The fix must
separate the two owners, then grant only the app owner.

## Background: the gp window and why it faults

RISC-V GCC routes "small" globals (size <= `-msmall-data-limit`, default 8 B) into
`.sdata`/`.sbss`, addressed as `gp + imm` where `imm = symbol - __global_pointer$` and `gp`
is a single register set once at boot (`startup.S`, `.option norelax; la gp, __global_pointer$`).
The psABI reach is `__global_pointer$ +/- 0x800` (4 KiB total).

Current `esp32c6.ld` / `virt.ld` (KICKOS_HAVE_MPU path) place the `.sdata`/`.sbss` CATCH-ALL
inside the kernel `.data`/`.bss`, right after `__kickos_code_end` and BELOW the app window,
with `PROVIDE(__global_pointer$ = . + 0x800)`. Measured on the linked esp32c6 cxxtest image:
`eh_globals`, `seen_objects`/`unseen_objects` (the DWARF FDE registry), `_impure_ptr` land at
~0x40820000, just below `__global_pointer$`=0x40820800 -- kernel-side, outside the app window
[0x40828000,0x40830000). A U-mode throw reads/writes them and faults under PMP.

The app's OWN objects already dodge this: `kickos_add_application` passes `-msmall-data-limit=0`
to the app TUs (`cmake/kickos.cmake`), so their globals leave the gp window and land in `.data`/
`.bss`, captured by the app-side `.appdata`/`.appbss` catch-all. The PREBUILT vendor libs are
not compiled that way, so their small-data stays in the (kernel-side) gp window.

## Evidence gathered (qemu-riscv, RISCStar rv32imac/ilp32 multilib, this tree)

1. **The KickOS libs DO emit gp small-data today** -- the crux of the "empty the kernel side"
   option. `objdump -h` on the built archives (KICKOS_HAVE_MPU=1):
   - `libkickos_arch_rv32imac.a`: `.sbss g_arch_current, g_arch_next, g_isr_depth, g_clint_msip`;
     `.sdata g_irq_masked, g_inject_line` -- i.e. the scheduler's MOST sensitive state (the
     current/next context pointers and the CLINT msip doorbell) live in the gp window.
   - `libkickos_kernel.a`: `.sbss g_kernel, g_default_user, g_console_panicking, g_led_on`.
   - `libkickos_chip_virt.a`: `.sdata SystemCoreClock`. `libkickos_lib.a`: none.
   So the shared gp window is NOT app-only. Handing it to U-mode without first emptying it
   would let an unprivileged thread overwrite `g_arch_current` / `g_clint_msip` -- total
   compromise. This is why a naive "grant the gp window app-side" is unsafe on its own.

2. **`-msmall-data-limit=0` empties the KickOS side.** Recompiling `arch_rv32imac.cc` with the
   flag moves `g_arch_current` to `.bss.g_arch_current` and `g_irq_masked` to `.data.g_irq_masked`
   (0 `.sdata`/`.sbss` sections) -- exactly the mechanism the app TUs already use. Fully in our
   control (our sources, our build flags); no library rebuild.

3. **No newlib small-data is kernel-referenced.** A freestanding (non-full-C++) selftest image
   pulls in NONE of newlib's small-data (`_impure_ptr`, `__malloc_av_`, `__sf`, reent): they
   appear only once libstdc++/libsupc++ is linked, i.e. dragged in by the APP's `operator new` /
   exception machinery, all U-mode. The kernel archives' only newlib undefined is `memset`
   (pure `.text`, no small-data). So in a full-C++ image EVERY gp small-data resident is either
   (a) KickOS-owned, movable out by (2), or (b) C++-runtime/newlib, app-side and U-mode-only.
   There is no third, kernel-shared class -- the reason the "empty + grant" fix is sound.

4. **The runtime residents that must land app-side** (`nm` on the full-C++ image): `eh_globals`,
   `__new_handler`, `__terminate_handler`, `__unexpected_handler`, `seen_objects`/`unseen_objects`
   (FDE registry), `DW.ref.__gxx_personality_v0`, `_impure_ptr` (small-data); plus `__malloc_av_`
   and friends which are already `.data`-sized (> limit) and already fall through to `.appdata`.

## Option evaluation

### Option 1 -- Rebuild the RISC-V C++ runtime (and newlib) `-msmall-data-limit=0`

Recompile libstdc++/libsupc++/libgcc (and libc, since `_impure_ptr`/malloc small-data also ride
the window) so their writable globals leave `.sdata`/`.sbss` and land in ordinary `.data`/`.bss`,
captured by the existing app-side catch-all.
- Fixes both boards: yes (backend-shared).
- PMP entries: +0.
- Context-switch: +0.
- Burden: HIGH and against the house rule. `-msmall-data-limit` is a per-TU compile flag baked
  into the prebuilt `.a`; changing it means recompiling the pinned RISCStar multilib
  (gcc/newlib/libstdc++ bootstrap), which breaks the "one pinned, reproducible prebuilt, no
  distro drift" guarantee the toolchain file states. It is a heavyweight, reproducibility-eroding
  change for an outcome that Option 4 achieves with a linker-script + build-flag edit and no
  rebuild. Rejected as the primary fix (kept only as the theoretical root-cause baseline).

### Option 2 -- Per-domain gp: two small-data windows, reload gp on the kernel<->user boundary

Give the kernel and the app SEPARATE gp windows and reload `gp` at the M<->U context boundary.
**Infeasible for a single-image link. Reasoning, from the actual code + the psABI:**

- `gp` is currently NOT saved/restored per thread -- it is a link-time constant set once in
  `_start` (`switch.S` header: "gp/tp are NOT saved"; the 128 B frame has no gp slot). Adding a
  per-domain gp would require a gp slot in the frame AND a `la gp, <kernel_gp>` (norelax) at the
  very top of `trap_entry`, because the trap handler (M-mode kernel) begins executing with
  whatever gp the interrupted U-mode thread had. That part is mechanically possible.

- **The killer is the single `__global_pointer$`.** A `gp`-relative reference's immediate is
  `symbol - __global_pointer$`, computed at link time against the ONE `__global_pointer$` symbol,
  and linker gp-relaxation (`.option relax`, on by default) rewrites absolute `la`/loads into
  `gp`-relative forms assuming that one fixed anchor. In a single ELF you cannot have kernel refs
  resolved against a kernel_gp and app refs against an app_gp: the linker knows only one anchor,
  so kernel `.sdata` and app `.sdata` are BOTH addressed relative to it and must BOTH sit within
  its +/- 0x800 reach -- i.e. back in ONE window. Reloading `gp` to a second runtime value makes
  one of the two precomputed-immediate sets address wild memory. Two anchors need two separate
  links (a distinct kernel ELF and app ELF); KickOS links the app against `libkickos*` into ONE
  image, so that escape hatch is unavailable.

- If instead you removed the kernel's gp dependency entirely (compile kernel `norelax` +
  `-msmall-data-limit=0` so it emits no `.sdata` and never reads `gp`), there is no kernel gp to
  preserve and nothing to reload -- which is exactly Option 4 with one gp. So Option 2 either is
  impossible (two live anchors) or degenerates into Option 4 (one anchor, kernel emits none).

- PMP: even in the degenerate case an app-side gp window is +0/+1 entries (same as Option 4);
  the gp-reload machinery adds context-switch cost and a `trap_entry` hazard window for no gain.

Rejected: no configuration of Option 2 beats Option 4, and the literal two-gp form does not link.

### Option 3 -- Compile the WHOLE system `-msmall-data-limit=0` (no gp small-data anywhere)

Passing the flag to every KickOS TU AND the app empties the KickOS side of the window, but the
PREBUILT vendor libs keep their `.sdata`/`.sbss` (their `.a` was compiled with the default limit).
Those residents (`eh_globals`, `_impure_ptr`, ...) still land in the gp catch-all wherever the
script puts it. So this alone does NOT relocate the runtime's small-data -- to move THAT you must
still either rebuild the libs (reduces to Option 1) or relocate the catch-all app-side (Option 4).
Also note: passing `-msmall-data-limit=0` to the APP is the wrong move -- the app's DWARF-EH refs
(`DW.ref.*`, LSDA datarel) resolve `gp`-relative, so keeping app small-data enabled is required
for unwinding (see `design-cxx-under-mpu.md`). Insufficient by itself.

### Option 4 -- Empty the kernel side, anchor gp INSIDE the app grant (RECOMMENDED)

Force `-msmall-data-limit=0` on the KickOS libs ONLY (kernel/arch/chip/lib), so their small-data
moves to `.data`/`.bss` and the gp window holds NOTHING kernel-owned (evidence 1+2). Then move the
`.sdata`/`.sbss` CATCH-ALL from the kernel `.data`/`.bss` into `.appdata`/`.appbss`, and set
`__global_pointer$` inside that block. Now the single gp window contains only app + C++-runtime
small-data (evidence 3+4) and is covered by the app grant. Keep the app compiled WITH small-data
(no `-msmall-data-limit=0` on the app) so unwinding still works.

- Fixes both boards: yes -- `esp32c6.ld` and `virt.ld` share the layout; the edit is identical.
- PMP entries: **+0** if the gp window folds into the existing `.appdata` NAPOT region (it is
  tiny -- the runtime small-data is well under 0x800, so `__global_pointer$` at the block's
  middle reaches all of it, and the whole thing fits inside the current 4 KiB `_appdata_size`).
  A separate gp-window PMP region is +1 if ever wanted, but is unnecessary.
- Context-switch: **~2 instrs** -- one gp, one `__global_pointer$`, gp-relaxation intact.
  Because gp now anchors LIVE app small-data, `switch.S` must force-reload gp on dispatch (see
  step 5) so a U-mode thread cannot corrupt the next thread's gp-relative addressing.
- Burden: a build-flag change on four KickOS libs + a linker-script move. No toolchain rebuild.
  Cost is a small KickOS code-size increase (absolute vs gp-relative addressing of kernel globals)
  and losing the ISA's small-data optimization for KickOS code -- negligible for a microkernel.

This is the layout `design-cxx-under-mpu.md` calls "gp-in-appdata" and reports proven on
qemu-riscv; this study's contribution is the option-space proof that it is the RIGHT one (Options
1/2/3 are dominated or infeasible) and the measured confirmation that step 1 is load-bearing.

## Recommended approach -- concrete steps

1. **cmake:** add `-msmall-data-limit=0` to the four KickOS RISC-V libs (kernel, arch_rv32imac,
   chip, lib) under `if(_arch STREQUAL rv32imac AND KICKOS_HAVE_MPU)` -- the same guard that
   already gates the app flag. Do NOT add it to the app (keep app small-data for unwinding).

2. **linker (`esp32c6.ld` + `virt.ld`, KICKOS_HAVE_MPU path):** remove `*(.sdata .sdata.* .sdata2.*)`
   from the kernel `.data` and `*(.sbss .sbss.*)` from the kernel `.bss`; add them to `.appdata`
   /`.appbss`; move `PROVIDE(__global_pointer$ = ...)` into the `.appdata` small-data sub-block.

3. **guard (security-critical):** the flag must be COMPLETE -- one KickOS global left in `.sdata`
   is now inside an app-granted region, a privilege-escalation vector. A targeted per-TU flag is
   fragile (a future kernel global silently re-populates the gp window). Keep it fleet-wide over
   all four libs AND add a CI gate: `nm`/`objdump` the KickOS `.a`s and assert zero `.sdata`/
   `.sbss` sections (mirrors the existing `__kernel_bss_end > __kernel_bss_start` selector guard).

4. **ASSERT gp reach:** add a linker `ASSERT` that the runtime small-data sub-block fits
   `__global_pointer$ +/- 0x800`, so a future runtime that grows its small-data fails loudly
   rather than mis-addressing.

5. **restore gp on dispatch (security-critical):** moving `__global_pointer$` into the live app
   grant means a malicious U-mode thread can set its own `gp` to any value; the next thread would
   then run its `gp`-relative small-data loads/stores at attacker-chosen addresses (defeating the
   per-thread-stack guarantee at U-level). `switch.S` does NOT save/restore gp per thread, so the
   restore epilogue (`.Lrestore` -- the choke point every dispatch path funnels through) must
   force `gp = __global_pointer$` (`.option norelax`) before every `mret`. gp is one link-time
   constant for the whole image and the kernel uses no gp-relative code, so the reload is a safe
   ~2-instruction self-heal on each switch.

## Proof-of-concept: the linker layout (sketch, not final)

Kernel side (emit nothing here once the libs are `-msmall-data-limit=0`; the catch-all is gone):

```
.data __kickos_code_end : ALIGN(8)
{
    _sdata = .;
    *libkickos_kernel.a:*(.data .data.*)
    *libkickos_arch_rv32imac.a:*(.data .data.*)
    *libkickos_chip_<board>.a:*(.data .data.*)
    *libkickos_lib.a:*(.data .data.*)
    /* no *(.sdata ...) here anymore: KickOS libs emit none */
    . = ALIGN(4);
    _edata = .;
} > RAM AT > RAM
```

App side (the gp window now lives inside the granted appdata region):

```
.appdata : ALIGN(_appdata_size)
{
    __kickos_appdata_start = .;
    *(.data .data.*)                         /* app + vendor .data (already app-side) */
    . = ALIGN(8);
    __sdata_start = .;
    PROVIDE(__global_pointer$ = . + 0x800);  /* gp: anchor inside the app grant */
    *(.sdata .sdata.* .sdata2.*)             /* ONLY runtime small-data reaches here now */
    . = ALIGN(4);
    __sdata_end = .;
} > RAM AT > RAM

.appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
{
    __kickos_appbss_start = .;
    *(.sbss .sbss.*)                         /* runtime small-bss (eh_globals, ...) */
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    _appdata_used_end = .;
    . = __kickos_appdata_start + _appdata_size;
    __kickos_appdata_end = .;
} > RAM

ASSERT(__sdata_end <= __global_pointer$ + 0x800 &&
       __sdata_start >= __global_pointer$ - 0x800,
       "KickOS: runtime small-data overflows the gp +/- 0x800 reach")
```

`arch_domain_static_regions` (`kernel/domain/domain.cc`) is UNCHANGED: it still reports the code
(RX) region and the `[__kickos_appdata_start, __kickos_appdata_end)` (RW-NX) region -- the gp
window is now inside the latter, so the PMP entry count is unchanged.

## Needs bench silicon to confirm

- **gp-relaxation with the anchor mid-`.appdata`.** The link succeeds and relaxation keeps a single
  anchor, but confirm on a real esp32c6 image that no relaxed reference goes out of reach after the
  anchor move (QEMU `virt` is the first gate; the C6 is the silicon gate).
- **PMP NAPOT granularity on the C6.** The `.appdata` region must stay a naturally-aligned power of
  two >= 8 B (`arch_mpu_region_encodable`); the C6's actual PMP grain (`pmpaddr` implemented bits)
  is a known bench-silicon item and bounds the minimum `_appdata_size`.
- **C6 PMP special-cases.** Already recorded: the C6 does not honor the all-ones-NAPOT
  match-everything idiom (TOR is used for the bootstrap entry). Re-verify the granted app region
  matches as expected under a real U-mode throw on silicon.

## See also

- `design-cxx-under-mpu.md` -- the broader full-C++-under-MPU experiment; reaches the same
  "gp-in-appdata" layout and reports it proven on qemu-riscv. This doc is the focused option-space
  justification (rules out per-domain gp) and the measurement that the KickOS libs emit gp
  small-data today (so emptying the kernel side is load-bearing, not cosmetic).
- `reference/porting.md` -- the `rv32imac` arch / PMP backend seam.
