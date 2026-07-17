<!-- SPDX-License-Identifier: CECILL-C -->
# Design: full C++ userspace under M2 MPU enforcement

Scope: make the full-C++ opt-in (exceptions + STL + RTTI, commit dc632bd) work for an
UNPRIVILEGED, MPU-isolated userspace thread -- the convergence the north star needs
(unprivileged C++ servers/drivers reached by IPC). Today `cxxtest` is gated
`AND NOT KICKOS_HAVE_MPU`; this doc scopes how to lift that gate, backed by a working
qemu-riscv experiment. PLAN, not landed -- experiment code lives only in the working tree.

## Verdict

**Feasible, and PROVEN on qemu-riscv.** An unprivileged PMP-isolated thread ran throw/catch
(with unwind of a non-trivial local dtor), `std::vector` over the heap, and `dynamic_cast`/
`typeid` -- then MPU-FAULTED on an ungranted kernel address, confirming PMP was enforcing the
whole time. The five gated blockers are all real; four are cheap, one (RISC-V `gp`/small-data)
is the hard part and is what the experiment had to solve.

The key structural facts that make it work:
- On DWARF-EH arches (RISC-V, RX) the EH tables (`.eh_frame`, `.gcc_except_table`) already
  sit inside the `.text` output section, i.e. inside the per-app **code region** granted RX --
  so unwinding *reads* them from a granted region for free. Same for RTTI `type_info`/vtables
  (`.rodata`, also in the code region).
- The runtime's *writable* state (heap, FDE registry, malloc bins, newlib reent, the libsupc++
  `eh_globals` + emergency pool) is the problem: it lands **kernel-side**, unreachable by the
  unprivileged thread. Fixing that is blockers #4 + the `gp` issue below.

## The region set an unprivileged full-C++ thread needs

An unprivileged thread already gets (kernel/thread/thread.cc + arch_domain_static_regions):
code (RX), app static-data (RW-NX), its own stack, and any granted domain/MMIO region. Full C++
adds nothing to the *count* -- everything folds into the two existing static regions:

| Runtime need | Section | Region it must land in | Static or dynamic |
|---|---|---|---|
| `.eh_frame` / `.gcc_except_table` (DWARF) | in `.text` | code (RX) | static, already granted |
| `.ARM.exidx` / `.ARM.extab` (ARM EHABI) | in `.text`/rodata | code (RX) | static, already granted |
| RTTI `type_info`, vtables | `.rodata` | code (RX) | static, already granted |
| landing pads | `.text` | code (RX) | static, already granted |
| libc heap arena (`s_heap`) | `.bss` (libkickos_user.a) | appdata (RW) | static placement; malloc carves it |
| FDE registry list heads (`seen_/unseen_objects`) | `.bss` (libgcc) | appdata (RW) | static |
| the malloc'd FDE `struct object` node | heap | appdata (RW) | dynamic -> from the granted heap |
| newlib malloc bins (`__malloc_av_`), reent (`_impure_*`) | `.data`/`.sdata` (libc) | appdata (RW) | static |
| libsupc++ `eh_globals`, `emergency_pool`, `__new_handler` | `.bss`/`.sbss` (libstdc++) | appdata (RW) | static |

So the whole EH+RTTI+heap footprint costs **0 extra regions**: RX tables ride the code region,
everything writable rides the one appdata region. Budget stays 3 (code+appdata+stack) of 8,
+1 per granted domain/MMIO region.

## The malloc'd FDE node (blocker #2)

On RISC-V/RX, `__register_frame(&__eh_frame_start)` registers the DWARF table; libgcc mallocs a
`struct object` to hold it. KickOS already calls `__register_frame` **at boot in `Reset_Handler`
(privileged)**, before any drop to unprivileged -- so the node is malloc'd from the libc heap
*while privileged*. Once that heap arena is in the granted appdata region (below), the node lands
there automatically. **No move needed, and no `__register_frame_info_bases`/static-object path
required** -- boot-time registration + a granted heap is sufficient (verified: the experiment
registers at boot and the unprivileged throw unwinds without faulting).

ARM EHABI has **no runtime registration at all**: `.ARM.exidx` is found via linker symbols
(`__exidx_start`/`__exidx_end`) in the code region. ARM sidesteps this blocker entirely.

## Code-region sizing (blocker #3)

The full-C++ image text overflows the per-app code window. Measured (qemu-riscv cxxtest/cxxumpu):
real code+rodata+eh_frame `_code_used_end` = **~77 KB** (of which `.eh_frame` ~26 KB). The C6
default `_code_size = 64 K` is too small -- **128 K** is needed. Per-arch, this depends on the
unit's region granularity:

- **RISC-V PMP (NAPOT):** strict power-of-two. 77 KB -> **128 K** region (51 KB wasted). Hard
  limit. C6 has 512 KB HP SRAM so 128 K code + 128 K appdata = 256 K is affordable but a big bite.
  qemu-virt (8 MB) is trivial.
- **ARM PMSA v7-M:** power-of-two, but 8 **sub-region-disable** bits let a 128 K region drop
  unused eighths -> less waste than PMP. 108 KB image -> 128 K region.
- **RX MPU:** base/end are **16-byte granular, NOT pow2** -> size the code region exactly. Best
  case for code sizing.
- **K64F SYSMPU:** region descriptors are **32-byte granular, arbitrary** -> also exact-sizable
  for SRAM; code is in flash granted as one RX window.

Which arches can host full-C++-under-enforcement TODAY vs need a bigger window: all can, once
`_code_size`/region is raised to 128 K (pow2 arches) or sized to fit (RX/K64F). The pow2 arches
just pay dead SRAM/flash for it.

## The selector gap (blocker #4) -- CONFIRMED root cause + fix

`*user*(.data .data.*)` in the chip .ld was believed to catch `libkickos_user.a`. **It does not.**
Evidence (map + nm on the qemu-riscv image): GNU ld matches a bare `*pattern*` against the
archive **member** name, not the archive path -- so `*user*` catches the directly-compiled app
object (its *build path* contains `user/apps/...`) but misses `libkickos_user.a(newlib_stubs.o)`
(member name `newlib_stubs.cc.obj`, no "user"). `s_heap` and `s_brk` landed kernel-side. And the
*toolchain* runtime (libc/libgcc/libstdc++) never had "user" in any name -- `__malloc_av_`,
`_impure_*`, `seen_/unseen_objects`, `eh_globals`, `emergency_pool` all landed kernel-side too.

**Fix (verified):** select by explicit archive with the `archive:member` colon syntax, per
runtime lib, not by path substring:
```
*libkickos_user.a:*(.data .data.*)  *libc.a:*(...)  *libgcc.a:*(...)  *libstdc++.a:*(...)
```
This moved every runtime global into the granted appdata region (nm-confirmed: all `>= appdata_start`
and `< appdata_end`, none kernel-side). This also fixes m2-review-followup #1 (path-substring
fragility) as a side effect -- the colon selector is path-independent.

Also close blocker #2's sibling from the review: gate an
`ASSERT(_appdata_used_end > __kickos_appdata_start, ...)` so a future zero-match is loud.

## The hard part: RISC-V `gp` / small-data vs the appdata region

This is the blocker the other docs did not name, and the one that cost the most:

1. **`-msmall-data-limit=0` breaks DWARF unwinding.** KickOS compiles unprivileged RISC-V apps
   `-msmall-data-limit=0` to route their globals out of the `gp` window (which lives kernel-side)
   into `.appdata`. But that flag on a `-fexceptions` TU makes `__cxa_throw` **hang in an
   unbounded FDE walk** (reproduced: privileged root, PMP bypassed, tables byte-identical to the
   working non-MPU image -- so not an isolation fault). Removing the flag fixes the throw. Cause:
   the app's DWARF EH refs (`DW.ref.*`, LSDA datarel encodings) resolve `gp`-relative; moving them
   away from `gp` corrupts the lookup. **The very mechanism used to place app globals in the
   granted region is incompatible with `-fexceptions`.**

2. **Moving the toolchain libs' `.sdata`/`.sbss` out of the `gp` window breaks boot.** newlib/
   libgcc reference their small data `gp`-relative; relocating it far from `gp` yields wild
   loads (verified: boot wedges in `_malloc_r`).

**Solution that works (proven):** stop fighting `gp` -- move `gp` itself.
- Anchor `__global_pointer$` **inside** the appdata region and route all app + toolchain-runtime
  `.data/.sdata/.bss/.sbss` there (via the colon selectors), so `gp`-relative small-data lands
  in-region and the app keeps small-data enabled (unwinding works).
- Compile the **KickOS libs** (kernel/arch/lib/user) `-msmall-data-limit=0` so they emit no
  small-data and never depend on `gp` -- freeing `gp` to serve only the app + full-C++ runtime.
- The app itself: **do NOT** pass `-msmall-data-limit=0` (that was the unwind-breaker); its small
  globals now land in the appdata `gp` window.

Cost: a **fleet-wide build change** (all KickOS RISC-V libs rebuilt `-msmall-data-limit=0`) --
RISC-V-only, and it slightly enlarges KickOS code (absolute vs gp-relative addressing). ARM/RX
do not have RISC-V's `gp` small-data model, so this whole section is RISC-V-specific.

One more RISC-V boot fix the experiment needed: `.appdata`'s LMA != VMA (it packs after `.text`
in the image but its VMA jumps to the 128 K code boundary), so `Reset_Handler` must **copy
`.appdata` LMA->VMA** before zeroing `.appbss` (mirrors the existing `.data` copy). Without it,
`__malloc_av_` reads uninitialized memory and malloc faults.

## Per-arch feasibility matrix

| Arch / unit | EH model | FDE reg. | `gp` issue | Code region | Region cost (EH+RTTI+heap) | Status |
|---|---|---|---|---|---|---|
| RISC-V PMP (virt, C6) | DWARF | boot `__register_frame` | **yes (solved: gp-in-appdata)** | pow2 128 K | +0 (folds into code+appdata) | **PROVEN on qemu-riscv** |
| ARM PMSA v7-M (XMC, F411) | EHABI (.exidx) | none (linker syms) | no | pow2 128 K (sub-regions help) | +0 | untested, expected easiest |
| RX MPU (RX72M) | DWARF | boot `__register_frame` | RX small-data TBD | exact (16 B granular) | +0 | untested; verify RX small-data model |
| K64F SYSMPU | EHABI (.exidx) | none | no | exact (32 B granular) | +0 for SRAM/data | untested; SRAM-only isolation, coarse peripherals |

Consumption of the 8-region budget is **+0** everywhere: RX/exidx tables ride the code region,
all writable runtime state rides the one appdata region. ARM (EHABI, no registration, no gp) is
the least-effort next target; RISC-V is done; RX needs its small-data model checked (does RXv3
GCC use a gp-like base register? if so, the same gp-in-appdata trick applies; if it addresses
absolutely, only the colon-selector fix is needed).

## Experiment (the evidence anchor)

qemu-riscv `virt`, `-DKICKOS_HAVE_MPU=1`, RISCStar newlib toolchain. App `cxxumpu`: privileged
root spawns an **unprivileged** worker (`kos::thread::spawn(..., privileged=false)`) that runs
full C++. Changes applied (working tree only):
- virt.ld: `_code_size`/`_appdata_size` = 128 K; colon-selectors routing app + libc/libgcc/
  libstdc++/libkickos_user `.data/.sdata/.bss/.sbss` into `.appdata`/`.appbss`; `__global_pointer$`
  anchored inside `.appdata`; `_appdata_lma` exported.
- chip_virt.cc `Reset_Handler`: copy `.appdata` LMA->VMA before zeroing `.appbss`.
- cmake: KickOS libs built `-msmall-data-limit=0`; app NOT (keep small-data for unwinding).

Result:
```
[umpu] worker UNPRIVILEGED: start
[umpu] PASS vector/heap
[umpu] PASS throw/catch
[umpu] PASS rtti
[umpu] ALL DONE
... (probe) MPU FAULT: task 'umpu' attempted write at 0x80041000 -- reported
```
The final line (an added ungranted-write probe) confirms PMP was enforcing throughout: the C++
runtime did all its work inside the granted code + appdata + stack regions, and stepping outside
faulted. This is the full convergence proof on one arch.

## Staged plan

- **S1 -- RISC-V land (done in spirit).** Promote the experiment's virt.ld + Reset_Handler copy +
  cmake changes to real (guarded by KICKOS_HAVE_MPU); add the zero-match ASSERT; un-gate `cxxtest`
  on qemu-riscv under MPU as an unprivileged-worker CI test. Decide the fleet-wide
  `-msmall-data-limit=0` on KickOS RISC-V libs (measure the code-size delta first).
- **S2 -- ARM PMSA (expected cheapest).** No gp, no registration: just raise the code region to
  128 K, add the colon-selectors for the data-side runtime globals to the ARM chip .ld files,
  ensure `.ARM.exidx`/`.ARM.extab` are in the granted code region. Prove on qemu-arm (mps2), then
  XMC/F411 silicon.
- **S3 -- RX MPU.** Confirm the RXv3 small-data/`gp`-analog model; apply the colon-selectors and
  (if RX has a gp-like base) the base-in-appdata trick; exact-size the code region. Prove on
  rx72m silicon.
- **S4 -- K64F SYSMPU.** EHABI + coarse SRAM regions; colon-selectors for the data side; code in
  flash granted RX. Peripheral isolation stays coarse (AIPS ceiling), unrelated to EH.
- **S5 -- multi-thread hardening (cross-cutting).** Today the runtime is single-full-C++-thread:
  `eh_globals`/`emergency_pool` are single globals and `__malloc_lock` is a weak no-op. Two
  unprivileged full-C++ threads throwing/allocating concurrently corrupt shared state. Needs real
  `__malloc_lock` (IrqLock or per-domain arena) + per-thread `eh_globals` before multi-threaded
  full-C++ under enforcement. Also note: all unprivileged threads currently share the ONE appdata
  region, so the EH runtime globals are shared across the whole unprivileged side -- consistent
  with today's model (per-thread stacks isolate, app static data is shared), but a real
  per-domain full-C++ story wants a per-domain appdata + a shared read-only FDE registry region.

## Confidence + hardest unsolved bit

- **High:** region-set design (+0 regions), the colon-selector fix, boot-time FDE registration
  from a granted heap, the `.appdata` LMA->VMA copy -- all verified end-to-end on qemu-riscv.
- **High:** code-region sizing numbers (measured).
- **Medium:** ARM/RX/K64F feasibility -- reasoned from the EH model + region granularity, not yet
  built. ARM is low-risk (EHABI removes two blockers); RX carries the one open question (its
  small-data model).
- **Hardest unsolved bit:** the **fleet-wide `-msmall-data-limit=0` on KickOS RISC-V libs**. It is
  the linchpin that frees `gp` for the app+runtime, but it is a broad build change with a
  code-size cost, and it is inelegant (the ISA's small-data optimization is sacrificed OS-wide to
  host the toolchain runtime). Worth measuring the delta and considering whether a narrower scope
  (only the TUs whose globals `gp`-collide) is achievable. Everything else is mechanical.
