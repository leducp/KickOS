<!-- SPDX-License-Identifier: CECILL-C -->
# Design: full C++ userspace under M2 MPU enforcement

Scope: make the full-C++ opt-in (exceptions + STL + RTTI, commit dc632bd) work for an
UNPRIVILEGED, MPU-isolated userspace thread -- the convergence the north star needs
(unprivileged C++ servers/drivers reached by IPC). `cxxtest` was gated
`AND NOT KICKOS_HAVE_MPU`; this doc scopes lifting that gate. The committed `cxxtest` now spawns
an UNPRIVILEGED worker (`kos::thread::spawn(cxx_worker, ..., privileged=false)`, 8 KB app-arena
stack) that runs the whole throw/catch/unwind + STL + RTTI body under the MPU. On qemu-riscv
(rv32imac PMP) that is RUN-PROVEN: `ctest -R qemu_riscv_cxxtest` is ALL PASS from the U-mode
worker -- the deterministic RISC-V/PMP gate. On the five silicon arches (frdmk64f, xmc4800-relax,
picopi, rx72m, esp32c6) the now-U-mode cxxtest is BUILD-VERIFIED here (links clean under
enforcement); the standing silicon ALL-PASS logs (2026-07-19) were the OLD privileged cxxtest =
the runtime COEXISTS with enforcement. A bench re-flash of the now-U-mode cxxtest is the
outstanding silicon U-mode proof.

## Verdict

**Feasible; the confined U-mode throw is RUN-PROVEN on qemu-riscv (rv32imac PMP); silicon is
privileged-coexistence run-proven + U-mode build-only.** cxxtest -- exceptions caught by exact
type and `std::exception` base, stack-unwind dtors, `std::vector`, `std::string`, virtual
dispatch, `dynamic_cast` hit+miss, `typeid` -- runs to completion under enforcement. The committed
cxxtest spawns an UNPRIVILEGED worker that throws/catches/unwinds under the MPU, and on qemu-riscv
it RUNS to ALL PASS (`ctest -R qemu_riscv_cxxtest`) -- so a confined unprivileged thread throwing
under PMP is now demonstrated by a standing in-tree test, not merely layout-verified. Option 4's
gp-in-appdata is thereby EXERCISED by a running confined test. On the five silicon arches spanning
all three EH models -- EHABI (frdmk64f, xmc4800-relax, picopi), SjLj (rx72m), DWARF (esp32c6) --
the standing ALL-PASS logs (2026-07-19) were the OLD privileged cxxtest: they prove the full-C++
runtime (libstdc++/libsupc++/newlib + the EH-table-homing layout) BOOTS AND RUNS while enforcement
is active (runtime/MPU coexistence). cxxtest is now U-mode on those boards too, but only
BUILD-VERIFIED here (links clean under enforcement) -- a bench re-flash of the now-U-mode cxxtest
is the outstanding silicon U-mode proof. The five gated blockers are all real; four are cheap, one
(RISC-V `gp`/small-data) is the hard part.

The fleet spans **three EH models** -- EHABI (ARM: K64F, XMC, rp2040), SjLj (RX72M), and DWARF
(RISC-V esp32c6/virt + Xtensa esp32). All three keep their EH tables in read-only sections that
already ride the per-app **code region** granted RX, so unwinding *reads* them for free:
- DWARF (RISC-V): `.eh_frame` + `.gcc_except_table` sit inside the `.text` output section.
- SjLj (RX72M): the setjmp/longjmp landing-pad tables (`.gcc_except_table`) are homed in ROM,
  inside the code region. No `.eh_frame`, no `__register_frame`.
- EHABI (ARM): `.ARM.exidx`/`.ARM.extab` ride the code region, found via linker symbols.

RTTI `type_info`/vtables (`.rodata`) also live in the code region.
- The runtime's *writable* state (heap, FDE registry, malloc bins, newlib reent, the libsupc++
  `eh_globals` + emergency pool) is the problem: it lands **kernel-side**, unreachable by the
  unprivileged thread. Fixing that is blockers #4 + the `gp` issue below.

## The region set an unprivileged full-C++ thread needs

An unprivileged thread already gets (kernel/thread/thread.cc + arch_domain_static_regions):
code (RX), app static-data (RW-NX), its own stack, and any granted domain/MMIO region. Full C++
adds nothing to the *count* -- everything folds into the two existing static regions:

| Runtime need | Section | Region it must land in | Static or dynamic |
|---|---|---|---|
| `.eh_frame` (DWARF) / `.gcc_except_table` (DWARF+SjLj) | in `.text`/ROM | code (RX) | static, already granted |
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

On DWARF arches (RISC-V), `__register_frame(&__eh_frame_start)` registers the DWARF table; libgcc mallocs a
`struct object` to hold it. KickOS already calls `__register_frame` **at boot in `Reset_Handler`
(privileged)**, before any drop to unprivileged -- so the node is malloc'd from the libc heap
*while privileged*. Once that heap arena is in the granted appdata region (below), the node lands
there automatically. **No move needed, and no `__register_frame_info_bases`/static-object path
required** -- boot-time registration + a granted heap is sufficient (verified: the experiment
registers at boot and the unprivileged throw unwinds without faulting).

ARM EHABI has **no runtime registration at all**: `.ARM.exidx` is found via linker symbols
(`__exidx_start`/`__exidx_end`) in the code region. ARM sidesteps this blocker entirely. RX SjLj
likewise registers no FDE list -- landing pads are reached via the SjLj context chain, not a
registered table -- so RX also sidesteps it, needing only a weak `__dso_handle` and an RX-mangled
`sbrk` (a C `sbrk` -> asm `_sbrk`, because RX's C `_sbrk` mangles to asm `__sbrk`).

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

On esp32c6 silicon this WAS the open item: the image links, boots, and registers the DWARF
frame, but the U-mode THROW path was blocked because the prebuilt libstdc++ EH writable state
(`eh_globals`, the FDE registry) landed in the shared `gp` small-data window, kernel-side. The
resolution is **"Option 4"** of docs/design-riscv-gp-split.md: empty the kernel `gp` side by
compiling the KickOS libs `-msmall-data-limit=0`, then anchor `__global_pointer$` inside the
`.appdata` window so that toolchain-runtime small-data lands in the granted region. LANDED and
nm-verified. On qemu-riscv Option 4 is now EXERCISED by a running confined U-mode throw
(`qemu_riscv_cxxtest` ALL PASS from the unprivileged worker) -- the deterministic gate. On esp32c6
silicon the standing 2026-07-19 cxxtest ALL PASS was the OLD privileged run (Option 4 layout-
correct, COEXISTS with PMP active); the now-U-mode cxxtest is build-verified here, pending a bench
re-flash for the silicon U-mode proof.

## Per-arch feasibility matrix

| Arch / unit | EH model | FDE reg. | `gp` issue | Code region | Region cost | Status |
|---|---|---|---|---|---|---|
| K64F SYSMPU (frdmk64f) | EHABI (.exidx) | none | no | exact (32 B granular) | +0 for SRAM/data | **frdmk64f silicon: priv-coexistence PASS; U-mode build-only** |
| ARM PMSA v7-M (XMC4800) | EHABI (.exidx) | none (linker syms) | no | pow2 128 K (sub-regions help) | +0 | **xmc4800-relax silicon: priv-coexistence PASS; U-mode build-only** |
| ARM PMSA (RP2040, Cortex-M0+/armv6m) | EHABI (.exidx) | none (linker syms) | no | pow2 128 K | +0 | **picopi silicon 2026-07-19: priv-coexistence 9/9 PASS; U-mode build-only** |
| RX MPU (RX72M, RXv3) | SjLj (.gcc_except_table in ROM) | none (SjLj chain) | no (RX has no gp small-data) | exact (16 B granular) | +0 | **rx72m silicon 2026-07-19: priv-coexistence 9/9 PASS; U-mode build-only** |
| RISC-V PMP (esp32c6, virt) | DWARF | boot `__register_frame` | **yes (solved: gp-in-appdata)** | pow2 128 K | +0 | **U-mode RUN-PROVEN on qemu-virt (`qemu_riscv_cxxtest` ALL PASS from the unprivileged worker); esp32c6 silicon privileged-coexistence ALL PASS 2026-07-19, U-mode build-only** |

Consumption of the 8-region budget is **+0** everywhere: EH/exidx tables ride the code region,
all writable runtime state rides the one appdata region. The confined U-mode throw is RUN-PROVEN
on qemu-riscv (rv32imac PMP): `qemu_riscv_cxxtest` is ALL PASS from the unprivileged worker. On
all FIVE silicon arches (K64F/SYSMPU, XMC4800/PMSA, RP2040/PMSA, RX72M/RX-MPU, esp32c6/RISC-V-PMP)
the standing 2026-07-19 ALL-PASS logs were the OLD privileged cxxtest: they prove the runtime
COEXISTS with enforcement active. cxxtest is now U-mode on those boards too but only build-verified
here (links clean); a bench re-flash is the outstanding silicon U-mode proof. RX turned out to
need neither `gp` work nor DWARF registration: RXv3 GCC addresses absolutely (no gp small-data
window) and its
SjLj EH registers no FDE list, so the colon-selector data-side fix plus a weak `__dso_handle` and
an RX-mangled `sbrk` sufficed.

## Experiment (the original evidence anchor -- now folded into the committed cxxtest)

qemu-riscv `virt`, `-DKICKOS_HAVE_MPU=1`, RISCStar newlib toolchain. The original throwaway app
`cxxumpu` -- privileged root spawns an **unprivileged** worker
(`kos::thread::spawn(..., privileged=false)`) that runs full C++ -- has since been folded into the
committed `cxxtest` (same spawn shape, now the standing `qemu_riscv_cxxtest` gate), so the
U-mode-confined claim now rests on a running in-tree test, not this record. The changes below are
all LANDED:
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
The final line (an added ungranted-write probe) confirmed PMP was enforcing throughout: the C++
runtime did all its work inside the granted code + appdata + stack regions, and stepping outside
faulted. That convergence is now a standing proof: the committed `cxxtest` spawns the same
unprivileged worker and `qemu_riscv_cxxtest` runs it to ALL PASS under PMP in CI.

## Staged plan

- **S1 -- RISC-V land (DONE).** The experiment's virt.ld + Reset_Handler copy + cmake changes are
  real (guarded by KICKOS_HAVE_MPU); the zero-match ASSERT is in; `cxxtest` is un-gated on
  qemu-riscv under MPU and runs as the `qemu_riscv_cxxtest` unprivileged-worker CI test (ALL PASS).
  Still open: decide the fleet-wide `-msmall-data-limit=0` on KickOS RISC-V libs (measure the
  code-size delta first).
- **S2 -- ARM PMSA (DONE, validated on xmc4800-relax + picopi silicon).** No gp, no registration:
  raise the code region to 128 K, add the colon-selectors for the data-side runtime globals to the
  ARM chip .ld files, ensure `.ARM.exidx`/`.ARM.extab` are in the granted code region. cxxtest
  passes under enforcement on XMC4800 and RP2040 (picopi 9/9).
- **S3 -- RX MPU (DONE, validated 2026-07-19 on rx72m).** RXv3 GCC has no gp small-data window
  (addresses absolutely) and uses SjLj EH (no `__register_frame`): the colon-selector data-side
  fix, a weak `__dso_handle`, and an RX-mangled `sbrk` (a C `sbrk` -> asm `_sbrk`, since RX's C
  `_sbrk` mangles to asm `__sbrk`) sufficed; code region exact-sized. cxxtest 9/9 PASS.
- **S4 -- K64F SYSMPU (DONE, validated on frdmk64f silicon).** EHABI + coarse SRAM regions;
  colon-selectors for the data side; code in flash granted RX. Peripheral isolation stays coarse
  (AIPS ceiling), unrelated to EH.
- **S5 -- multi-thread hardening (cross-cutting).** Today the runtime is single-full-C++-thread:
  `eh_globals`/`emergency_pool` are single globals and `__malloc_lock` is a weak no-op. Two
  unprivileged full-C++ threads throwing/allocating concurrently corrupt shared state. Needs real
  `__malloc_lock` (IrqLock or per-domain arena) + per-thread `eh_globals` before multi-threaded
  full-C++ under enforcement. Also note: all unprivileged threads currently share the ONE appdata
  region, so the EH runtime globals are shared across the whole unprivileged side -- consistent
  with today's model (per-thread stacks isolate, app static data is shared), but a real
  per-domain full-C++ story wants a per-domain appdata + a shared read-only FDE registry region.

## Confidence + hardest unsolved bit

- **Proven (qemu-riscv, U-mode):** region-set design (+0 regions), the colon-selector fix,
  boot-time FDE registration from a granted heap, the `.appdata` LMA->VMA copy -- verified
  end-to-end by the committed `cxxtest` unprivileged worker (`qemu_riscv_cxxtest` ALL PASS under
  PMP).
- **High:** code-region sizing numbers (measured).
- **High (runtime silicon-validated privileged; U-mode build-only):** ARM/RX/K64F feasibility --
  the standing silicon logs are the OLD privileged cxxtest running to completion with enforcement
  active on frdmk64f (SYSMPU), xmc4800-relax + picopi (PMSA, EHABI), and rx72m (RX MPU, SjLj) --
  runtime/MPU coexistence. The now-U-mode cxxtest links clean on all four; a bench re-flash is the
  outstanding silicon U-mode proof. The one RX open question (its small-data model) is closed:
  RXv3 addresses absolutely, no gp analog.
- **Hardest unsolved bit:** the **fleet-wide `-msmall-data-limit=0` on KickOS RISC-V libs**. It is
  the linchpin that frees `gp` for the app+runtime, but it is a broad build change with a
  code-size cost, and it is inelegant (the ISA's small-data optimization is sacrificed OS-wide to
  host the toolchain runtime). Worth measuring the delta and considering whether a narrower scope
  (only the TUs whose globals `gp`-collide) is achievable. Everything else is mechanical.
