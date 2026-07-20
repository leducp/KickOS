# RP2350 (Cortex-M33) MPU: the ARMv8-M PMSAv8 backend

Status: DESIGN / PLAN. Not implemented. SILICON-PENDING (the Waveshare RP2350
Pi-Zero has never booted). Requires a fable design review before any merge.

This scopes the memory-protection backend for the RP2350's Cortex-M33. It is the
register-level follow-on to `docs/design-rp2350.md` "DEFERRED (a)", written after
inspecting the shared ARM PMSA backend to answer one question precisely: does the
already-landed v7-M `arch_mpu_apply` work on the M33, or does the M33 need a
distinct ARMv8-M backend?

## Verdict

**The RP2350 M33 needs a new ARMv8-M PMSAv8 `arch_mpu_apply`. The shared v7-M
PMSA backend does NOT work on it and must never be pointed at the M33's MPU.**

The M33 implements PMSAv8 only; there is no v7-M-compatible MPU mode on an ARMv8-M
core. The v7-M backend does not merely "under-program" the M33 -- it writes
v7-M-shaped values into a register pair whose meaning changed, producing garbage
regions that fail closed on the first unprivileged access. Evidence below.

## Where enforcement lives today, and why it is wrong on the M33

The enforcement backend is the weak `arch_mpu_apply` in
`arch/arm/common/arch_arm_common.cc` (guarded `#if KICKOS_HAVE_MPU`). It is
compiled into `kickos_arch_armv7m` (`arch/CMakeLists.txt`: the armv7m library
lists `arm/common/arch_arm_common.cc`). The RP2350 chip reuses the `armv7m` arch
verbatim (`chip_rp2350.cc` calls `kickos_armv7m_init`; `rp2350.ld` links
`libkickos_arch_armv7m.a`) and ships **no** MPU override and **no** `mpu.cmake`.
So an enforcement build for this chip would link the v7-M PMSA backend.

The v7-M backend programs, per region (`arch_arm_common.cc`):

```
reg32(MPU_RNR)  = i;                                   // 0xE000ED98
reg32(MPU_RBAR) = base & ~0x1F;                        // 0xE000ED9C
reg32(MPU_RASR) = ENABLE | (size_field << 1) | AP | mem-type | XN;  // 0xE000EDA0
```

with `MPU_RASR` at `0xE000EDA0` encoding SIZE[5:1] (region = 2^(SIZE+1)),
AP[26:24], TEX/S/C/B, XN[28] (see `arch/arm/common/regs.h`). This is the classic
v6-M/v7-M PMSA layout, correct for the M0+/M3/M4/M7 boards (rp2040, stm32f103,
stm32f411, xmc4800).

On PMSAv8 (the M33) the same core addresses mean different things:

| Addr | v7-M (M3/M4/M7) | v8-M (M33) |
|------|-----------------|------------|
| 0xE000ED90 | `MPU_TYPE` (DREGION[15:8]) | `MPU_TYPE` (same) |
| 0xE000ED94 | `MPU_CTRL` (EN/HFNMIENA/PRIVDEFENA) | `MPU_CTRL` (same layout) |
| 0xE000ED98 | `MPU_RNR` | `MPU_RNR` (same) |
| 0xE000ED9C | `MPU_RBAR` = base[31:5] + SIZE via RASR | `MPU_RBAR` = base[31:5] + SH[4:3] + AP[2:1] + XN[0] |
| 0xE000EDA0 | `MPU_RASR` = SIZE/AP/TEX/S/C/B/XN | **`MPU_RLAR`** = limit[31:5] + AttrIndx[3:1] + EN[0] |
| 0xE000EDC0/C4 | -- | `MPU_MAIR0/1` (8 attribute bytes, indexed by AttrIndx) |

`MPU_TYPE`, `MPU_CTRL`, `MPU_RNR` are compatible, so `MPU_CTRL = ENABLE |
PRIVDEFENA` still turns the unit on. The **region descriptors** are the failure:

1. **RBAR.** The v7-M path writes `base & ~0x1F`. On v8-M the cleared low 5 bits
   are not "reserved" -- they are SH[4:3], AP[2:1], XN[0]. So every region gets
   SH=0, **AP=0b00 (RW, privileged-only -- no unprivileged access)**, XN=0. An
   unprivileged thread therefore has **no** access to its own stack/data region:
   the first user load/store faults MemManage. Enforcement does not "sort of work"
   -- it denies the owner.

2. **RLAR misread as RASR.** The v7-M path writes its RASR value to `0xE000EDA0`,
   which on v8-M is `MPU_RLAR`. `MPU_RASR_ENABLE` is bit 0, which happens to be
   `MPU_RLAR.EN` -- so the region enables. But `limit[31:5]` then comes from the
   upper bits of the RASR value (AP/TEX/S/C/B), i.e. a **limit address unrelated
   to base+size**, and `AttrIndx[3:1]` comes from `(size_field << 1)`. The region
   covers `[base .. garbage]` with an attribute index into an **unprogrammed
   MAIR** (reset MAIR = 0 = Device-nGnRnE for every slot).

3. **No MAIR programming.** PMSAv8 splits type from permission via MAIR indirection;
   the v7-M path never touches MAIR0/1, so all memory is treated as strongly-ordered
   device -- wrong even where an address happens to land in-bounds.

Net: pointing the v7-M backend at the M33 produces enabled regions with
privileged-only permissions and nonsense limits. It is not a trivial-tweak case;
it is a semantically different unit. Hence a new backend.

(The `KICKOS_CHIP_ENFORCES_MPU` fail-loud floor in the top `CMakeLists.txt` protects
us today: RP2350 has no `mpu.cmake`, so `KICKOS_HAVE_MPU=1` on `pizero2350` is
*rejected at configure time* rather than silently linking the wrong backend. The
port is honest -- it claims no enforcement. This doc is what lifts that.)

## The seam is already sufficient: prove {base,size,attr} maps to PMSAv8

The frozen region contract is `struct arch_mpu_region { uintptr_t base; size_t
size; uint32_t attr; }` (`arch/include/kickos/arch/arch.h`), where `attr` is an OR
of `ARCH_MPU_{R,W,X,DEV}` and denotes **unprivileged** rights (supervisor comes
from the PRIVDEFENA background region). The project discipline: no per-arch field
may leak into this struct. PMSAv8 needs nothing more:

- **base -> RBAR.BASE[31:5]** (`base & ~0x1F`; base is 32-byte aligned by the
  allocator / linker, see below).
- **base+size -> RLAR.LIMIT[31:5]**, inclusive top: `limit = (base + size - 1) &
  ~0x1F`. This is the whole story that RASR's `size_field` told on v7-M -- PMSAv8
  states the top address directly, so **no power-of-two size and no
  natural-alignment gap are needed**.
- **attr -> RBAR.AP[2:1] + RBAR.XN[0] + RLAR.AttrIndx[3:1]:**
  - `ARCH_MPU_X` (code): AP = RO-any (`0b11`), XN = 0, AttrIndx = NORMAL slot.
  - `ARCH_MPU_DEV` (MMIO): AP = RW-any (`0b01`), XN = 1, AttrIndx = DEVICE slot.
  - default (data/stack): AP = RW-any (`0b01`), XN = 1, AttrIndx = NORMAL slot.
  ("any" = unprivileged access permitted; the privileged kernel reaches everything
  via PRIVDEFENA regardless, matching the v7-M `AP_RW`/`AP_RO` intent.)

AP[2:1] encoding used above (ARMv8-M ARM): `00`=RW priv-only, `01`=RW any,
`10`=RO priv-only, `11`=RO any. So `ARCH_MPU_R` alone (no W) on a data region maps
to `11` (RO any); the encoder keys off W and X exactly like `mpu_rasr` keys off
`ARCH_MPU_X`/`ARCH_MPU_DEV` today.

MAIR is a **one-time** setup, not per-region: program two slots once (index 0 =
Normal Write-Back/Write-Allocate `0xFF`, index 1 = Device-nGnRE `0x04`) into
MAIR0, and have `AttrIndx` select 0 or 1. This mirrors the RX/PMP backends' one-time
enable step; nothing per-switch beyond RBAR/RLAR pairs.

Conclusion: `{base,size,attr}` is provably sufficient for PMSAv8. No struct change.

## The arch-seam decision: strong override, reuse the armv7m arch

ARMv8-M Mainline is a superset of v7-M for everything KickOS's arch layer touches:
BASEPRI critical section, DWT cycle counter, SysTick, NVIC, PendSV/SVCall context
switch and syscall trampoline are all present and identical. `switch.S`,
`arch_armv7m.cc`, and the BASEPRI band are correct on the M33 unchanged (this is
why `chip_rp2350.cc` reuses `armv7m` verbatim and why the port already runs the
console path). **Only the MPU differs.** So a whole `armv8m` arch directory would
duplicate a large, identical arch to swap one file -- rejected.

The established pattern is that the MPU backend is a **weak symbol overridden per
chip** (architecture.md: "MPU is chip-specific, not arch-specific"). K64F already
does this: `chip_mk64f.cc` provides a strong `arch_mpu_apply` (SYSMPU) that wins
over the weak v7-M one in the same link (`arch/arm/chip/mk64f/`). The strong
definition in the chip library overrides the weak one in `kickos_arch_armv7m`.

Recommended shape (avoids per-chip copy-paste, since PMSAv8 is shared by *every*
ARMv8-M chip -- RP2350, and future nRF5340 / STM32U5 / STM32H5 M33 parts):

- New file **`arch/arm/common/arch_arm_pmsav8.cc`** providing strong
  `arch_mpu_apply`, `arch_mpu_region_encodable`, and (if it must differ)
  `arch_mpu_min_region`, plus PMSAv8 register defs in `arch/arm/common/regs_v8m.h`
  (RBAR/RLAR field encodings + MAIR bases 0xE000EDC0/C4). This file is **not** in
  the always-compiled `kickos_arch_armv7m` source list.
- The chip opts in through its `mpu.cmake` (the existing per-chip enforcement
  opt-in): `arch/arm/chip/rp2350/mpu.cmake` sets `KICKOS_CHIP_ENFORCES_MPU ON`
  **and** `target_sources(<chip lib> PRIVATE .../arch_arm_pmsav8.cc)` (path via a
  cached arch-source var), so the strong PMSAv8 symbols are pulled into the RP2350
  link and override the weak v7-M ones. A v7-M chip's `mpu.cmake` never adds it, so
  its v7-M weak backend stands.

This keeps: (a) the armv7m arch reused for everything non-MPU; (b) the v7-M weak
backend untouched and still correct for M0+/M3/M4/M7; (c) the two backends
side-by-side selected by presence-in-link, not by an `#ifdef` fork inside one
function; (d) zero per-arch field in `arch_mpu_region`. An alternative -- a single
`arch_mpu_apply` in `arch_arm_common.cc` with an `#if KICKOS_ARM_PMSAV8` fork -- is
viable and smaller-diff, but couples two register layouts in one TU and risks the
wrong branch being built; the separate-TU strong-override is cleaner and matches
the K64F precedent. Pick the fork only if fable review prefers minimal file count.

## `arch_mpu_region_encodable` and `arch_mpu_min_region` on PMSAv8

- `arch_mpu_min_region()` stays **32** (PMSAv8 granule is 32 bytes, same as v7-M).
- `arch_mpu_region_encodable(base,size)` becomes the **byte-granular** form, NOT the
  pow2 form -- PMSAv8 takes an arbitrary 32-byte-aligned `[base, base+size)`:
  `size >= 32 and (base & 31)==0 and ((base+size) & 31)==0`. This matches the
  branch `arch.h` already documents for "byte-granular archs (SYSMPU 32B / RX 16B)"
  -- PMSAv8 is exactly that shape at a 32-byte granule. So an MMIO grant no longer
  needs pow2 padding (which on v7-M over-granted neighbours or forced fail-closed).

Note the shared `arch_ram_region_size()` (`arch.h`) still pow2-shapes RAM regions
whenever `arch_mpu_min_region() != 0`. That remains **correct** on PMSAv8 (a pow2
range is a valid arbitrary range), just not minimal -- the allocator wastes the
same padding the v7-M chips do. Relaxing `arch_ram_region_size` to 32-byte-granular
for PMSAv8 is a **follow-on optimization**, out of scope for first enforcement; get
it correct first, then reclaim the padding. This is the payoff `design-rp2350.md`
flags ("deletes the pow2 `.appdata` window machinery") -- realizable, but stage it.

## Region budget

`MPU_TYPE.DREGION` reports the count; the M33 as configured on RP2350 implements
**8 regions** (read it, do not hard-code). Budget is identical to the fleet and
fits: code(RX) + kernel/app data(RW-NX) + per-thread stack + optional MMIO grant +
guard, within 8. The one-time MAIR setup consumes no region slots.

## Linker (`rp2350.ld`) work, when enforcement lands

`rp2350.ld` today deliberately omits the `.appdata` window (it says so, lines 21-24)
and already exports `__kickos_code_start/_end` (the flash XIP code region) and
`__kickos_ram_start/_end`. The PMSAv8 enforcement layout is **simpler** than the
v6/v7 pow2 `.appdata` machinery (rp2040.ld): because RLAR takes an arbitrary limit,
an app data/bss range is just two 32-byte-aligned symbols `(start,end)` fed to
RBAR/RLAR -- no `ALIGN(pow2)` on the block, no size-class padding. Add, guarded
`#if KICKOS_HAVE_MPU` (following the rx72m.ld pattern in memory: gate the block, no
`_sidata_app` line outside the guard -- that was the mk64f landmine):

- an app-data/app-bss range with `__kickos_appdata_start/_end` (32-byte aligned),
  its copy/zero-table entries, and an overflow ASSERT;
- keep `__kickos_code_*` ungated (cheap symbols, harmless when MPU off).

Do not carve the window now: it is dead weight until the backend exists, and this
doc's verdict is that the backend is real work, not a scaffolding pass.

## Fault path

PMSAv8 violations raise **MemManage** with MMFSR/MMFAR exactly as v7-M (SHCSR
MEMFAULTENA, CFSR MMFSR[7:0], MMFAR + MMARVALID). The shared
`kickos_armv7m_fault_report` already labels a set MMFSR byte as "MPU FAULT" and
prints MMFAR -- **no fault-path change needed** for the M33. The `mpu_fault`
selftest (ungranted write -> MemManage) is the silicon acceptance gate.

## SMP interaction (M5 endgame -- noted, NOT designed here)

The RP2350 is the M5 SMP target: dual M33 with real LDREX/STREX exclusives. The MPU
is **per-core, banked** -- each M33 has its own MPU register file at the same core
addresses (0xE000EDxx is core-local). Two design choices here must not paint the
SMP story into a corner:

- **Keep `arch_mpu_apply` operating on "the current core's MPU" only.** It writes
  core-local registers with no core index; that is already SMP-correct provided the
  switch path that calls it runs on the core the thread lands on. Do not add a
  global/shared MPU cache; the K64F-style "skip if region set unchanged" static
  cache (see the RX bring-up notes) would be **per-core state** under SMP and must
  become core-local (or be omitted) -- flag for M5, do not add it now.
- **MAIR one-time setup must run once per core**, not once globally -- fold it into
  the per-core bring-up (`kickos_armv7m_init` analogue), not a boot-once path.

No SMP mechanism is designed in this doc; these are guardrails so the single-core
PMSAv8 backend does not embed a single-core assumption (a shared static region
cache) that M5 would have to unwind.

## Plan of record

1. `arch/arm/common/regs_v8m.h` -- PMSAv8 RBAR/RLAR field encodings, MAIR0/1 bases.
2. `arch/arm/common/arch_arm_pmsav8.cc` -- strong `arch_mpu_apply` (one-time MAIR +
   enable; per-switch RBAR/RLAR pairs, disable unused up to DREGION),
   `arch_mpu_region_encodable` (32-byte-granular), `arch_mpu_min_region` (32).
3. `arch/arm/chip/rp2350/mpu.cmake` -- `KICKOS_CHIP_ENFORCES_MPU ON` +
   `target_sources` the PMSAv8 TU into the chip lib.
4. `rp2350.ld` -- `#if KICKOS_HAVE_MPU` `.appdata`/`.appbss` range + init tables.
5. Board guard: add `pizero2350` to the enforcement-capable list in the top
   `CMakeLists.txt` fail-loud message.
6. Silicon: `mpu_fault` + `selftest` under `KICKOS_HAVE_MPU=1`. First boot ever for
   this board -- expect to debug boot before MPU. fable review before merge.

Confidence to verify against silicon first (the ARM PMSA had two K64F silicon
surprises): AP[2:1] "any vs priv-only" bit sense; RLAR limit inclusivity at the
32-byte granule (off-by-one page); MAIR slot values actually yielding cacheable
Normal for XIP flash + SRAM.
