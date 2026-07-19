<!-- SPDX-License-Identifier: CECILL-C -->
# M2 review follow-ups (fable 10-angle review, this session)

Verdict: **needs minor fixes** -- NO confirmed isolation escape (fail-closed
throughout), NO Critical. The enforcement backends (SYSMPU/PMSA/PMP), the K64F
supervisor-field fix, the both-master grant, the PMP NAPOT encoder, the fail-closed
non-pow2 drop, the guard-word probe, the de-dup/hoist (ODR-clean), and the
zero-overhead story all verified sound. Findings are build-robustness + docs +
one cross-arch label drift + the HW-unproven RX register risk.

## Status
- [x] #3 invariants.md stale for M2 -- FIXED this session.
- [x] #1 `*user*` selector path-substring fragile -- FIXED: the 6 enforcement chip
      linker scripts adopt mk64f's archive:member colon-inversion (path-independent).
      Silicon-confirmed xmc4800/esp32c6 selftest 20/20.
- [x] #2 "matched nothing" ASSERT claimed but absent -- FIXED: kernel-side selector-death
      ASSERT added to all 6 scripts (RISC-V brackets the closed-set .bss only).
- [x] #4 RISC-V U-mode instruction-access fault (mcause=1) not labelled MPU FAULT -- FIXED.
- [x] #5 RX backend rounds (masks) misaligned regions instead of skipping -- FIXED: skip
      an unrepresentable region (slot V=0), fail-closed like ARM/PMP. RX HW re-run pending
      (board not on the bench).
- [x] #6 K64F peripheral-gating -- RESOLVED on silicon (see below): SYSMPU does NOT gate
      peripherals; the AIPS bridge does (coarse, per-slot, not per-thread).
- [ ] nits (below)

## #1 -- `.appdata` `*user*` selector is path-substring fragile (Major, PLAUSIBLE)
All chip .ld files group app globals via `*user*(.data .data.*)`. GNU ld matches the
wildcard against the object/archive PATH on the link line. Relies on nothing but the
app + libkickos_user.a containing "user". A build under a path containing "user"
(e.g. `/home/user/...`, common on CI) makes foreign object paths match -> pulls
kernel/lib `.data/.bss` into the unprivileged-granted region. Over-match overflows
`_appdata_size` -> the pow2 ASSERT trips -> loud (path-dependent) build break; a small
foreign object that fits 4K would silently widen U-mode reach (not a kernel-data
escape -- libkickos_kernel.a has no "user"). Fix: select by an explicit section tag,
not path substring -- compile user objects into a named section (e.g. `.user.data`/
`.user.bss` via a section attribute or a per-TU flag) and group THAT; path-independent
+ intentional. At minimum, document the build-path constraint.

## #2 -- the empty-match case is claimed-guarded but is not (Major, CONFIRMED)
Every chip .ld's ASSERT is only the overflow check
`_appdata_used_end <= __kickos_appdata_start + _appdata_size`; the comment claims a
zero-match is caught, but empty -> `used_end == start` -> `start <= start+4K` passes
silently. If a refactor makes `*user*` match nothing, app globals fall to the kernel
catch-all and every unprivileged thread faults on its first global (fail-closed, but
NO build signal, and the comment misleads). Fix: gate an
`ASSERT(_appdata_used_end > __kickos_appdata_start, ...)` on "app declares globals",
or delete the misleading half of the comment. (A global-free app must not trip it.)

## #4 -- RISC-V U-mode instruction-access fault not labelled MPU FAULT (Minor, CONFIRMED)
`arch/riscv/rv32imac/arch_rv32imac.cc:432-436` routes only mcause 5 (load) + 7 (store)
from U-mode to `kickos_isr_fault` ("MPU FAULT: task ..."). A U-mode instruction-fetch
violation is mcause 1, which falls through to the generic dump. ARM labels IACCVIOL as
MPU FAULT too. Fix: include `mcause == 1` in the from_user routing (mtval = faulting
PC, matches the contract). Safe either way (terminates); presentation/consistency.

## #5 -- RX backend rounds misaligned regions instead of skipping (Minor, PLAUSIBLE, build-only)
`arch/rx/rxv3/arch_rxv3.cc:486-511` admits a region on `size >= 16` then MASKs
base/end with `MPU_PAGE_MASK`, rather than SKIPPING a misaligned/non-page region as
ARM (arch_arm_common.cc:199-208) and PMP (arch_rv32imac.cc:248-253) do. Deviates from
the `mpu-apply-on-every-switch-in` fail-closed rule on the one HW-unproven backend.
Fix: verify base and base+size are 16-byte aligned; skip (leave V=0) otherwise.

## #6 -- K64F peripheral-gating -- RESOLVED on silicon (was: readiness row omits the caveat)
The first MMIO driver (k64drv, Stage 2) answered this on the FRDM-K64F. Result: **SYSMPU
does NOT gate AIPS peripheral-bridge accesses under user mode.** An unprivileged store to a
PIT register (inside its granted SYSMPU window) took an imprecise BusFault while the
privileged bring-up's stores to the same PIT succeeded and SYSMPU latched no error
(CESR SPERR clear). Peripherals are gated by the **AIPS bridge PACR** instead -- by
privilege+master, per 4 KB slot, resetting to supervisor-only. Clearing the slot's PACR
SP bit opens it to ALL user code (coarse, NOT per-domain). SRAM/domain isolation
(the 17/17 result) is unaffected. The full fleet peripheral-isolation matrix + the
CPU-side-vs-bus-slave-side principle are now in `reference/architecture.md` (Memory domains)
and `book/peripheral-isolation-and-the-hardware-ceiling.md`.

## Nitpicks
- rx72m.ld `.appdata ORIGIN(RAM)` with RAM ORIGIN=0 -> first app global at address 0
  (`&global == nullptr`); harmless freestanding, latent null-check surprise.
- RISC-V unprivileged + libc: `-msmall-data-limit=0` routes APP globals to .appdata,
  but libc/libgcc internal globals (reentrancy, malloc arena) stay kernel-side -> an
  unprivileged riscv thread calling libc faults touching them. Fine for mpu_fault; a
  constraint for real unprivileged drivers.
- `-msmall-data-limit=0` gating keys on `KICKOS_ARCH` in user/CMakeLists.txt:16 but the
  board-resolved `_arch` in cmake/kickos.cmake:220 -- safe today, asymmetry invites
  drift; consider one helper.
