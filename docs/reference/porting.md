<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS porting guide

This is the practical companion to `architecture.md`: how a new target implements
the `arch.h` seam. It also records the **M1 ARMv7-M spike** -- the design and
feasibility conclusion for the one mechanism the roadmap flagged as make-or-break.

The porting seam is `arch/include/kickos/arch/arch.h` (authoritative). A target
provides two halves:

- an **arch** backend (`arch/<arch>/`, e.g. `arch/arm/armv7m/`) -- the ISA-generic
  machinery: context switch, syscall trap, critical section, core timer/clock, NVIC;
- a **chip** backend (`arch/arm/chip/<chip>/`) -- the hardware edges: reset/startup
  + vector table, clock tree, UART console, `arch_init`/`arch_shutdown`, the
  linker script (which defines the user-RAM region `__kickos_ram_start/_end`), and
  `SystemCoreClock` (defined in the chip C, not the linker script). Optionally a
  chip may override `arch_diag_led_init`/`arch_diag_led_set` (the kernel
  diagnostic LED, `kdiag_led_*`); both have no-op fallback TUs
  (`arch/common/arch_diag_led_{init,set}_default.cc`), so a board with no known LED
  just leaves them out.

No KickOS seam is a weak symbol. An optional seam's fallback body lives ALONE in a
translation unit named `<symbol>_default.cc` that defines EXACTLY ONE global symbol, and a
backend's own definition must sit in an always-anchored archive member -- the rule is stated
in `arch/CMakeLists.txt` (lines 11-71) and detailed under *Privileged register write* below.

### Fault-reporter contract (panic must survive console handover)

A chip's fault/exception reporter MUST, before ANY console output: call
`kpanic_enter()` first, and emit only via `kprintf`/`console_emit` -- NEVER
`arch_console_write_sync` directly. Both are load-bearing once a board enables
console *device handover* (a userspace driver takes the UART): `kpanic_enter`'s
reclaim branch re-seizes + re-inits the relinquished UART (`arch_console_reclaim`),
and `console_emit` honors `ConsoleState` (drops the chip path while USER_OWNED,
routes to the polled writer once RECLAIMED). A reporter that prints before
`kpanic_enter`, or pokes the sync writer directly, would emit to a relinquished
(possibly dead) UART and SILENTLY lose the panic banner -- the worst failure mode
in the system. All six current fault reporters satisfy this; a new arch port must
too. Additionally, before a board turns on handover it must supply a real
`arch_console_reclaim` body (the generic fallback,
`arch/common/arch_console_reclaim_default.cc`, is a no-op -- a silent
reclaim failure otherwise); today only XMC (USIC) and K64F (UART0) have one.

### Adding a board/chip (the three edit points)

1. `boards/<board>/board.cmake` -- the board descriptor: one file setting
   `KICKOS_ARCH`, `KICKOS_CHIP` (empty for the sim), and `KICKOS_MCPU`
   (`-mcpu/-mfpu/-mfloat-abi`). This is the **single source of truth** for the
   board -> {arch, chip, CPU} triple: the ARM cross toolchain includes it
   pre-`project()` for the `-mcpu` baseline, and the build's board resolver
   (`cmake/kickos.cmake`, `kickos_load_board_descriptor`) includes the same file
   for arch + chip. The two can never disagree. (This replaced the old triplet:
   a `-mcpu` ladder in the toolchain + `kickos_resolve_board`/`kickos_resolve_chip`
   ladders in `kickos.cmake`.)
2. `CMakePresets.json` -- add a configure + build preset (only for boards that
   actually build/link today).
3. `arch/arm/chip/<chip>/` -- the chip sources (`*.cc`, `*.S`, auto-globbed), a
   linker script named exactly `<chip>.ld`, and `include/kickos/board_config.h`
   with the board facts. CMake derives the dir from the chip name, puts it on the
   include path, and installs it -- **no root-CMake edit needed** (this used to be a
   silently-failing step).

`boards/<board>/` is also where per-board overrides of a shared chip live: a
board-specific `include/kickos/board_config.h` (selected EXCLUSIVELY over the chip
default -- see below) and/or a `<chip>.ld` linker override -- proven on the `stm32f411` pair
(f411disco + blackpill) and the `stm32f103` pair (bluepill + bluepill-c8).

`board_config.h` is a pure-`#define` header (so `startup.S` includes it too),
pulled in by `kernel/include/kickos/config/{board,system}.h`. It sets:
   - `KICKOS_MAX_IRQ` -- the chip's NVIC line count. The `startup.S` vector table
     derives its `.rept` from this exact macro, so the vector table and the kernel
     IRQ table are **one fact**, not the same number copied into two files (the old
     silent-skew hazard). Defaults to 32 if the header is absent.
   - `KICKOS_MAX_THREADS` + the idle/root/user stack sizes, sized to the chip's
     SRAM. Too big and the link fails on the linker-script RAM `ASSERT`.
   Every knob is `#ifndef`-guarded, so a `-DKICKOS_...=` on the CMake line still
   overrides for a one-off (edit the header for a persistent/shipped change). The
   sim has no chip header and falls through to the config-header defaults.

`boards/<board>/include` is selected **EXCLUSIVELY** over the chip include dir
(`CMakeLists.txt`, `KICKOS_BOARD_INCLUDE_DIR`: it is an if/else, not an append), and both
headers use the same include guard, so a board that ships its own `board_config.h` never
sees the chip's -- not even for a knob it did not restate. `bluepill-c8` hits this: it has
`boards/bluepill-c8/include/kickos/board_config.h` with `KICKOS_USER_STACK_SIZE 2048` /
`KICKOS_ROOT_STACK_SIZE 2048`, so `arch/arm/chip/stm32f103/include/kickos/board_config.h`
-- sized to the low-density 10 KiB floor at 1024/1024 -- is NEVER seen. Copy the chip
header and edit it; do not write a partial override and expect the chip's values behind it.

**An `AT` clause in `<chip>.ld` is only valid if the LOADER honours LMA.** This is
LOADER-dependent, not arch-dependent, and both sides of the decision look identical in the
linker script.

Broken case, fixed in M4.5.6: `arch/riscv/chip/esp32c6/esp32c6.ld` linked `.data`
`> RAM AT > RAM` with an explicit VMA at `ORIGIN + 128K` while the load counter kept
counting from the end of `.text`, so `_sidata` landed OUTSIDE every loaded segment. esptool
`elf2image` builds the bootable image from VMAs, so those bytes were never loaded, and
`Reset_Handler` then copied uninitialised SRAM over the `.data` the ROM had already placed
correctly. Measured on silicon: `sidata=40806810 sdata=40820000 ... src+1c=ee95242c
dst+1c=00000000`. Because the corrupting bytes are uninitialised SRAM, the symptom varies
by die and power-on history -- it presented as a board that reached `entry 0x40800000` and
then went silent, and bisecting found nothing. The fix was to drop the `AT` and add
`ASSERT(_sidata == _sdata)` so it cannot regress. `arch/xtensa/chip/esp32/esp32.ld`
carried the same latent construct and took the same assert (image-neutral there).

CORRECT case, and the trap: `arch/riscv/chip/virt/virt.ld` carries the SAME
`> RAM AT > RAM` construct and the SAME divergence (`_sidata = 0x80010ce8` against
`_sdata = 0x80020000`), and there it is RIGHT -- QEMU's ELF loader places each segment at
its PhysAddr, so `.data`'s bytes really are at the LMA and the `Reset_Handler` copy does
real work. The `qemu-riscv` `+MPU` 15/15 gate is the proof. **Do NOT add the
`_sidata == _sdata` assert to `virt.ld`**; a comment in that file already says so.

So: decide which side your loader is on BEFORE writing an `AT` clause, and pin the decision
with an `ASSERT` either way. `docs/reference/boards.md`, *M4.5.6*, holds the wire evidence.

A chip whose flash boot needs a checksummed second stage (RP2040 boot2) adds a
fifth point: `cmake/<chip>_checksum.py` plus a `boot2.S`/`boot2.ld` in the chip
dir; the build wires the multi-stage boot2 image automatically (keyed on
`${KICKOS_CHIP}`). A chip on a non-ARM ISA additionally needs a new arch backend +
toolchain file -- see `arch/xtensa/` (ESP32) or `arch/rx/` (RX72M) for worked examples.

Status -- five arch backends (**armv7m** Cortex-M3/M4/M4F/M7/M33, **armv6m** Cortex-M0/M0+,
**rxv3** Renesas RX72M, **lx6** Xtensa/ESP32, **rv32imac** RISC-V) across the chips below.
"MPU" is whether the chip ships an enforcement backend (`arch/<family>/chip/<chip>/mpu.cmake`);
where it does, cross-domain trapping is silicon-proven unless the row says otherwise:

| Chip | Board | Core | MPU | Validation |
|------|-------|------|-----|------------|
| `mps2` | qemu / qemu-m33 / qemu-m7 / qemu-m3 | M4F / M33 / M7 / M3 | PMSAv7 + PMSAv8 | QEMU (four runnable CI gates, **plus runtime enforcement gates** on both PMSA revisions) |
| `nrf51` | microbit | M0 | -- | QEMU (runnable CI gate) |
| `virt` | qemu-riscv | RV32IMAC | PMP | QEMU (runnable CI gate, **plus a runtime enforcement gate**) |
| `xmc4800` | xmc4800-relax | M4F | PMSAv7 | **hardware** (LED + USIC VCOM console over the buffered ring; enforcement + the canonical per-thread peripheral-isolation proof) |
| `stm32f411` | f411disco / blackpill | M4F | PMSAv7 | **hardware** (LED + UART + ping-pong; enforcement selftest + `mpu_fault` MemManage denial + an unprivileged root, all on `f411disco` 2026-07-29). Witnessed on one of the two boards; `blackpill` shares this backend and was not re-run |
| `stm32f302` | f302nucleo | M4 | -- | **hardware** (LED PB13 + console; the full suite at the `f302nucleo-st` provisioning -- 63 ok / 0 not ok / 5 skipped on 16 KiB SRAM, measured at `124b68c`). Not an enforcement target: the F302R8 line has no MPU, so `arch_mpu_min_region()` returns 0 (`arch/arm/chip/stm32f302/chip_stm32f302.cc:321`) |
| `stm32f103` | bluepill-c8 | M3 | -- | **hardware** (F103 port HW-proven on the now-retired 10 K clone, 2026-07-14; RAM-limited selftest; c8 build-only). No MPU: the degraded privilege-only build |
| `rp2040` | picopi | M0+ | PMSAv6-M | **hardware** (selftest over UART0/GP0; v6-M cross-domain fault silicon-proven 2026-07-19) |
| `rp2350` | pizero2350 | M33 | **PMSAv8** | **hardware** (enforcement selftest + `mpu_fault` MemManage denial + bench/soak). Reuses the `armv7m` backend verbatim; only the MPU descriptor shape differs |
| `mk64f` | frdmk64f | M4F | SYSMPU | **hardware** (revalidated 2026-07-15: full selftest + buffered console ring; **unprivileged root on the FULL service list**, 2026-07-29). SYSMPU enforces SRAM/domains but is bus-slave-side, so it cannot gate peripherals; the `AIPS0` PACR half of that is `arch_periph_enable`'s, below |
| `imxrt1062` | teensy41 | **M7** | PMSAv7 + fixed | **hardware** (enforcement selftest + soak). The only speculating core: needs the fixed-region wrap (`../design-teensy-mpu-hang.md`) |
| `rx72m` | rx72m | RXv3 | RX MPU | **hardware** (selftest + SCI6 console; DPFPU switch; enforcement + a granted peripheral window). **No CI gate** -- see below |
| `esp32` | esp32-wroom | Xtensa LX6 | -- | **hardware** (selftest + console, 240 MHz). No per-task MPU and no privilege split |
| `esp32c6` | esp32c6-wroom | RV32IMAC | PMP | **hardware** (selftest + buffered ring console; first real peripheral IRQ; enforcement + peripheral isolation). Peripheral access also needs the one-time bus-side APM open, which `arch_init` programs at boot |
| `sam3x8e` | due | M3 | -- | port proven on silicon (2026-07-09); test unit retired (peripheral-I/O fault) |

Build-only chips are verified by construction (register review + image
inspection); flash to a board to confirm. `apps/blink` is a no-UART LED smoke
test available on every board with a known LED.

**What CI does and does not re-check.** Not every row above is defended by a green CI run, and a
porter needs to know which. Enforcement is gated at RUNTIME on the sim (`mprotect`), `virt` (PMP)
and the four `mps2` images (full TAP suite as unprivileged threads plus a real MemManage denial;
PMSAv7 on the M4/M7/M3, PMSAv8 on the M33). The silicon boards get an enforcement **build** sweep
instead, and it is worth having on its own -- it compiles their enforcement-only link surface,
including `arch_reserved_blocks`, which has **no fallback TU on purpose**, so an enforcing port
that forgets to declare its reserved set fails to LINK rather than leaving a silent open hole --
but their chip-specific trapping (SYSMPU, the M7 anti-speculation wrap, PMSAv6) stays
silicon-proven. Xtensa is build-only (no upstream ESP32 machine model), and
**Renesas RX has no CI gate at all**: RX72M needs `-misa=v3`/`-mdfpu`, which exist only in the
registration-gated Renesas GNURX build. So a change to the arch seam is unverified for RX until
you build it yourself. Per-ISA detail: `boards.md` ("CI coverage & cross toolchains").

### Cross toolchains (before you configure anything)

A cross build resolves its compiler through a per-family hint variable, seeded from the
environment, overridable with `-D`, falling back to `PATH` when empty:
`KICKOS_ARM_TOOLCHAIN_BIN`, `KICKOS_RISCV_TOOLCHAIN_BIN`, `KICKOS_RX_TOOLCHAIN_BIN`,
`KICKOS_XTENSA_BIN`. No toolchain file carries a default compiler path, so **keep local pins in
`.session/env.sh` (gitignored) and `source` it first**; CI sets the same variables and puts the
pinned tarball's bin on `PATH`. Both routes matter: CMake's `try_compile` re-reads the toolchain
file with a fresh cache, so it inherits the environment and `PATH` but never a `-D` cache entry --
which is why each toolchain file re-exports the resolved value into the environment.

**The hint is convenience; the capability check is the safety net.** `find_program` HINTS fall
through to `PATH` when the hinted directory is absent, so a fresh clone on another host can
silently resolve a distro cross-gcc -- and Debian's `arm-none-eabi-g++` is C-only picolibc with no
`libstdc++`/`libsupc++` for any multilib, which used to surface ~40 build steps later as
`fatal error: exception: No such file or directory`. The ARM and RISC-V toolchain files therefore
probe the compiler they actually resolved (`cmake/toolchain-cxx-runtime-check.cmake`) and refuse
it at configure time, naming the compiler, the multilib, what was missing, the override variable
and the official tarball URL. The probes carry the board's own `-mcpu`/`-march`, because a
toolchain can ship `libstdc++` for one multilib and not another and the default multilib would
hide that; picolibc is tested *positively* (via `__PICOLIBC__`) because Debian's build also
defines `__NEWLIB__`, so inferring it from newlib's absence would pass it. **A new ARM or RISC-V
port inherits this for free.** RX and Xtensa deliberately skip it -- neither has a same-name
C-only twin on `PATH` to fall through to, so `find_program(... REQUIRED)` is already loud enough.

### Pin-function config (`arch_pinmux_set`)

One-shot init-time pin muxing is an arch/chip-seam entry
(`arch/include/kickos/arch/arch.h`): `int arch_pinmux_set(uint32_t port, uint32_t
pin, uint32_t func)`, reached from userspace as syscall `KOS_SYS_PINMUX_SET` (33),
privileged-only. `func` is a **chip-opaque** function code (the PORT/PCR/IOCR
encoding), so the ABI `{port, pin, func}` stays vendor-neutral while each backend
owns its own encoding. Returns 0, `-KOS_EINVAL` (out of range), or `-KOS_EBUSY` (a
kernel-owned pin the backend refuses). The **declining fallback**
(`arch/common/arch_pinmux_set_default.cc`) **returns `-KOS_ENOSYS`**, so
a non-empty board pin-map fails LOUD on a chip with no backend rather than silently
mis-muxing.

Backends exist for `mk64f`, `xmc4800`, `rp2040`, `rp2350`, `esp32c6`, `rx72m`,
`stm32f411`, `stm32f103`, `stm32f302`, `sam3x8e`, `imxrt1062`, `esp32`. `nrf51`,
`mps2`, and `virt` keep the declining fallback (no central mux block -- per-peripheral
PSEL, emulated, or virtual). Per-backend caveats: `stm32f103` covers default-mapped
peripherals only (AFIO_MAPR remap out of scope); `imxrt1062` keys `port`=GPIO-bank /
`pin`=bit against a PARTIAL pad table (a hole returns `-KOS_EINVAL`); `esp32c6` packs
BOTH of that family's mux stages into `func` -- the IO_MUX pad word in bits `[15:0]`,
the GPIO-matrix out-sel signal index in `[31:24]` behind an arm bit `[23]` -- so the
kernel-owned-pin refusal covers the matrix and not just the pad (`esp32` still muxes
the pad only); `rx72m` likewise packs both RX stages -- the MPC `PmnPFS` byte
(`PSEL[5:0]` | `ISEL` | `ASEL`) in `[7:0]`, the `PORTm.PMR` bit value in `[8]`, an arm
bit for the `PFS` write in `[9]` -- and does the MPC `PWPR` unlock plus the mandated
"clear `PMR`, write `PSEL`, restore `PMR`" order itself, since a `PFS` write outside
that bracket is silently dropped. `port` there is the DENSE register index 0..0x17
(PORT0..PORTQ; `PORTG` is 0x10), and package pin holes are not modelled -- a pin absent
from the package is accepted and writes a reserved bit. The board
supplies the routing as a `kos_board_pinmap` table the init service walks before the
service list; the init DAG is pinmux -> service list -> app.

`stm32f411` encodes MODER verbatim in `[1:0]`, the AF number in `[7:4]`, and an
output-preset-high arm in `[8]` (`PINMUX_OUT_HIGH`, chip-local at
`arch/arm/chip/stm32f411/chip_stm32f411.cc:389`): the preset writes `BSRR` **before**
`MODER`, so a pin never drives the `ODR` reset level for even one cycle on its way to
its idle level -- which is what an active-low chip select needs. The bit is refused
`-KOS_EINVAL` on any non-output mode. `OSPEEDR` and `PUPDR` stay unreachable through
this seam. `stm32f302` shares the first two fields and has no third, which is the point
of `func` being chip-opaque: a new field is a per-chip encoding change, not an ABI one.

### Peripheral enable (`arch_periph_enable`)

Owning a peripheral window is not enough to reach it: the block must be clocked, and
the bus must stop classifying the access as supervisor-only. Both live in chip-global
registers that no per-thread MPU grant can ever cover, so they are a seam of their own
-- `int arch_periph_enable(uintptr_t base)` (`arch/include/kickos/arch/arch.h:105`),
reached from userspace as syscall `KOS_SYS_PERIPH_ENABLE` (39) via
`kos_periph_enable`. It ungates the clock and drops the bus-side supervisor-protect for
the register block at `base`, and it is **idempotent**, so a driver calls it as its
first act without knowing whether the board already did.

`base` is the register-**BLOCK** base and must match a per-chip table **EXACTLY**; a
backend never range-matches. Both the register and the bit are derived from `base`, so
the ABI carries no register address and no bit number, and a caller cannot name a
shared block's register or bit. The seam returns 0, `-KOS_EINVAL` (no entry for that
base) or `-KOS_ENOSYS`. The **declining fallback returns `-KOS_ENOSYS`**
(`arch/common/arch_periph_enable_default.cc`) rather than 0, so a driver whose block really is
gated fails LOUD on a chip with no backend instead of reading registers that BusFault.

**The gate is possession, not authority.** The syscall refuses `-KOS_EPERM` unless the
caller holds a live region whose `attr` carries `ARCH_MPU_DEV` and whose `base` equals
the requested base exactly -- containment does not count, so a window covering the
block is not a licence to enable a different one
(`kernel/syscall/syscall_mem.cc` (`caller_holds_mmio_block`), dispatch at
`kernel/syscall/syscall.cc` (the `KOS_SYS_PERIPH_ENABLE` arm)). No
capability bit grants this; **holding the window IS the credential**, which is why an
unprivileged driver can enable its own block while root cannot enable one it does not
hold. A **privileged** caller short-circuits the check and is always allowed, but the
only privileged thread in a running image is `idle`, which calls no syscalls -- so every
caller that can reach this seam, root included, goes through the possession check.

**The containment rule decides whether a base gets an entry at all.** A base is tabled
only where the bus gate's granularity is *contained by* the block the window covers.
Where the coarsest available gate would also open kernel-reserved registers, the base
is **refused** instead -- an absent entry is a decision, not an omission, and a porter
adding one must check this before the datasheet. The K64F PIT is the worked case:
`AIPS0` classifies per 4 KiB slot, so clearing SP for a granted channel-2 window
(`0x40037120`) would also expose the chained ch0+ch1 pair `arch_clock_now` runs on --
which `arch_reserved_blocks` protects by address. So the PIT gets no entry and
`pit_clock_init` gates it at boot instead (`arch/arm/chip/mk64f/chip_mk64f.cc:498`).

Backends exist for **two** chips; every other chip keeps the fallback, deliberately
including `esp32c6` (its one-time bus-side APM open is programmed by `arch_init`, not
per block) and `rx72m`:

| Chip | Block | Clock gate | Bus protect |
|------|-------|------------|-------------|
| `mk64f` | UART0 `0x4006A000` | `SIM_SCGC4` bit 10 | `AIPS0` slot 106, PACR `0x40000064` bit 22 |
| `mk64f` | DSPI0 `0x4002C000` | `SIM_SCGC6` bit 12 | `AIPS0` slot 44, PACR `0x40000044` bit 14 |
| `stm32f411` | SPI1 `0x40013000` | `RCC_APB2ENR` bit 12 | -- none exists for this bus |

The `mk64f` PACR register and bit are **computed** from `base` (`slot_of` / `pacr_of` /
`pacr_sp_bit`, `arch/arm/chip/mk64f/regs/aips.h`) rather than tabled, and the clock is
ungated before the protect is dropped. `stm32f411` is clock-only: no
privilege-classification register exists for that bus in this tree. That is a
legitimate backend shape -- the seam does not promise both halves, only that whatever
the chip has for that block is done.

A porter's in-env check is `periph_enable_unheld` (`selftest`, unguarded so it runs on
every board): it spawns an unprivileged worker holding no DEV window and requires
`-KOS_EPERM`, then self-grants a non-DEV region at the exact base and requires
`-KOS_EPERM` again, pinning the `ARCH_MPU_DEV` filter. The **positive** arm has no
in-env carrier at all: the host sim defines no `arch_periph_enable` (it keeps the declining
fallback), and the ONE window its `arch_mpu_region_encodable` admits
(`arch/sim/sim.cc`) is the fake register block the write seam below needs -- every other
address, and every other shape of that one, still fails closed. That arm is silicon-only,
and `c6blink` and `rxdrv` carry probes for it.

### Privileged register write (`arch_periph_reg_write`)

The other member of the same seam family, and the one for a bus that classifies the
WRITE side **per register** rather than per block. There the window is readable and
mostly writable, but a few registers discard an unprivileged store **silently** -- no
fault, read-back unchanged. `int arch_periph_reg_write(uintptr_t base, uintptr_t offset,
uint32_t value)` (`arch/include/kickos/arch/arch.h`) performs that one store privileged,
reached from userspace as syscall `KOS_SYS_PERIPH_REG_WRITE` (42) via
`kos_periph_reg_write`.

Two backends exist in-tree. `xmc4800` is the real one. The **sim** is the second, and its
purpose is a CI gate (`arch/sim/sim.cc`: `SIM_PVREG_BASES`, `SIM_PVREG_WINDOW`,
`SIM_PRIV_WRITE_REGS`). It models a write-PV-only block over real host pages: a **64 KiB**
window (`0x10000`) taken from the FIRST of FIVE candidate bases that `MAP_FIXED_NOREPLACE`
accepts -- `0x40000000` (1 GiB), `0x100000000` (4 GiB), `0x400000000` (16 GiB),
`0x10000000000` (1 TiB), `0x100000000000` (16 TiB) -- and PUBLISHED at init, so nothing
assumes a fixed host address. All five are valid addresses, decades apart, so no host
mapping or ASLR layout takes the whole set; `selftest`'s `periph_reg_write_mask` walks the
same list in the same order, and a drift between the two shows up as every candidate
refused, never as a pass. The allowlist has NO base column -- there is only ever one block
-- and one of its two entries sits deliberately BEYOND the grantable window, reproducing
the containment hazard on the host. 64 KiB is the window because every host page size in
practice (4, 16 and 64 KiB) divides it, so "`mprotect` describes this exactly" holds by
construction rather than by the host happening to use 4 KiB pages.

Porting a chip means writing the table; the mechanism is already gated there, and six
mutations each came out red on a distinct check: mask widened, silent trim, refuse-and-act,
containment dropped, alignment dropped, wrap dropped. What the host still cannot establish
is three things, and each stays a per-chip MEASUREMENT: the bus PV classification (that an
unprivileged store is silently DISCARDED -- `pvprobe` only), that a tabled block is
CLOCKED, and any chip's actual mask column.

**The gate is a STRICTER possession predicate** than `arch_periph_enable`'s.
`caller_holds_mmio_reg(base, offset)` (`kernel/syscall/syscall_mem.cc`) first matches a
live `ARCH_MPU_DEV | ARCH_MPU_R | ARCH_MPU_W` region whose base equals `base` EXACTLY,
then requires that region to **CONTAIN** `[base + offset, +4)`. No authority bit;
privileged callers short-circuit. The containment half is not a refinement, it is what
makes the possession check bound anything: matching the base alone lets a 32-byte window
at a block's start reach every register the chip table names anywhere in that block
(MEASURED before the fix -- a holder of 32 bytes at `0x40000000` passed the gate at
offsets `0x40`, `0x1000` and `0x0FFFFFFF`, and at an offset that wrapped 2^32). On the
XMC4800 that was a live confused deputy: a 32-byte window at `0x40030200` covers
`FDR`/`BRG` but not `CCR` at `+0x040`, so a holder that could neither read nor write
`CCR` could have the kernel write it privileged.

**Malformed requests are refused ahead of possession.** The offset must be 4-ALIGNED and
`base + offset` must not wrap; both answer `-KOS_EINVAL` whatever the caller holds,
because they are malformed rather than unauthorised. `arch_periph_enable`'s helper and
semantics are untouched by this -- it takes a block base and has no offset to bound.

What is deliberately NOT true is that a holder gains the block: the chip carries an
**ALLOWLIST of exact `(base, offset)` pairs** and refuses `-KOS_EINVAL` for anything
else. A per-block entry would hand back exactly what the bus classification withholds,
so the granularity is the register, never a range.

**"One register" is not "only the authority the driver needs", so each entry ALSO carries a
per-entry VALUE MASK.** `(value & ~mask) != 0` answers `-KOS_EINVAL` **before the store**
(`arch/arm/chip/xmc4800/chip_xmc4800.cc`, `arch_periph_reg_write` against
`PRIV_WRITE_REGS`): REFUSED WHOLE, never trimmed and never read-modify-written, because a
silently dropped configuration bit is the failure class the read-back exists to catch. On
`xmc4800`, `CCR` grants `MODE[3:0]`, `RIEN` and `AIEN` -- 6 bits of 32, with `RIEN`/`AIEN`
deliberate because `xmcssc` arms them last -- and withholds `TBIEN`, `HPCEN`, `PM`,
`RSIEN`, `DLIEN`, `TSIEN` and `BRGIEN`. `FDR` grants `STEP` and `DM`. `BRG` grants every
writable field, where the mask withholds only read-only and reserved bits and so buys
little; it is there so no entry carries a blanket word. A `static_assert` pins each
composed grant against the word it must equal, so a field-mask edit in the chip's register
header cannot widen a grant silently.

Silicon witness (`.session/m456-silicon/b2-pvprobe.log`, `xmc4800-relax` at commit
`270b6fa`):

```
[pvprobe] mask refusal: CCR|TBIEN rc=-22 (want -22), pre=0xc001 post=0xc001 unchanged
```

`pre == post` is the load-bearing half: the off-mask bit was not trimmed away and stored
without it, the whole word was refused.

**A tabled block must also be CLOCKED.** The contract REQUIRES it -- quoted from
`arch/include/kickos/arch/arch.h`: *"An entry's block MUST be CLOCKED whenever the syscall
can reach it"* -- though no backend CHECKS it. A store into a clock-gated block faults
INSIDE the privileged store, in the kernel's own frame, and that reaches
`kfault_terminate`: whole-system death from one syscall, i.e. an unprivileged caller's
one-syscall system-kill primitive. Not live today only because `kickos_xmc_usic_init()`
runs unconditionally from `arch_init` so USIC0 is never gated; a `U1C0`/`U2C0` entry
(gated at reset behind `CGATCLR1`) would arm it. A porter adding an entry for a block that
can be gated must either ungate it at `arch_init` or have the backend check.

Returns 0, `-KOS_EPERM` (the possession gate above, decided in the syscall layer),
`-KOS_EINVAL` (misaligned, wrapping, or not on the allowlist), or `-KOS_ENOSYS` (no
backend on this chip). `-KOS_ENOSYS`, not 0: a caller reaching for this seam has a
register the bus refuses it, so a success answer would report a store that never
happened. Every failure is a checked errno; nothing here faults.

**The default is NOT a weak symbol** -- and neither is any other seam fallback in the tree.
It is a one-symbol archive member, `arch/common/arch_periph_reg_write_default.cc`, compiled
into every `kickos_arch_*` library. The rule is stated canonically in `arch/CMakeLists.txt`
(lines 11-71), and it is LINKER behaviour that every Unix-like linker and MSVC `.lib` share,
not a language guarantee. A second symbol in that TU would drag the member in
unconditionally and collide on every chip with a backend.

**The load-bearing invariant is on the BACKEND side: a chip's definition must live in an
always-ANCHORED archive member** -- one the link pulls for some other reason
(`chip_<chip>.cc`, force-loaded via `-u g_isr_vector` -> `startup.S` -> `Reset_Handler`;
`arch/arm/chip/xmc4800/usic_uart.cc`, pulled for `arch_console_tx_backend`). Scan order
(`kickos_arch_*` after `kickos_chip_*` in the rescan group) is only a BACKSTOP: MEASURED
with the group reversed, the link still resolves correctly from the anchored chip member.
**A chip that puts its definition in a dedicated TU nothing else references gets NEITHER
protection -- the fallback resolves the reference first and the board SILENTLY DECLINES at
runtime.** Proved by mutation: the group reversed plus `arch_idle_wait` moved into an
unreferenced TU linked with ZERO diagnostics, `objdump` showing the fallback's `wfi`
instead of the chip's `nop`.

`tests/check_seam_defaults.sh` (ctest `seam_defaults`, run on EVERY board) gates it in four
legs, all four mutation-proved: the one-symbol rule, plus no fallback in `kickos_kernel`; for
a seam a backend defines, the fallback member ABSENT from the link map with the backend's
member present -- the anchoring leg; for a seam no backend defines, the fallback member
present in the map's inclusion list for that exact symbol, and a board that resolves no seam
from a fallback at all fails too, so the gate cannot go vacuous; and zero weak symbols
outside `tests/weak_allowlist.txt`.

| Chip | Block | Register | Per-entry value mask | Why it needs the seam |
|------|-------|----------|----------------------|-----------------------|
| `xmc4800` | USIC0 CH1 `0x40030200` | `FDR` `0x010` | `0x0000C3FF` (`STEP[9:0]`, `DM[15:14]`) | RM Table 18-20 marks it `Write = PV` with no `U`; the divider is the whole setting the driver needs |
| `xmc4800` | USIC0 CH1 `0x40030200` | `BRG` `0x014` | `0xF3FF7FDB` (every writable field) | same table row; only read-only and reserved bits are withheld |
| `xmc4800` | USIC0 CH1 `0x40030200` | `CCR` `0x040` | `0x0000C00F` (`MODE[3:0]`, `RIEN`, `AIEN`) | same table row; the mask is what keeps the channel's other interrupt enables out of the grant |

`xmc4800`'s U0C0 (`0x40030000`) has **no** entry: the kernel owns the console channel's
baud and enable (`usic_uart.cc`), and an absent entry is a refusal, not an omission --
the same rule as the K64F PIT above.

A porter's in-env check is `periph_reg_write_unheld` (`selftest`, unguarded), THREE arms:

1. A caller holding no DEV window requires `-KOS_EPERM`.
2. A holder of a real DEV window at a base the chip does not table requires a REFUSAL
   that is neither `-KOS_EPERM` (the gate passed) nor 0 (nothing may accept an untabled
   address) -- so on every board with no backend it pins the default's `-KOS_ENOSYS` at
   runtime. It finds the window **by attempting the spawn over candidate bases**, NOT
   with `kos_grant_probe`: the probe syscall needs `KICKOS_ENABLE_SELFTEST`, and using it
   would silently drop this arm on a production-ABI build.
3. The OFFSET BOUND: the same holder asks for an offset that is 4-aligned and that the
   chip table COULD name, but that lies outside the window it holds, and requires
   `-KOS_EPERM`. Arm 3 is the one the earlier shape could not express -- `PRW_OFFSET` was
   `0x2`, deliberately unaligned and unnameable, so it never tested containment. It is now
   `0x4` and the unnameability guarantee moved from the offset to the BASE.

All three verdicts collapse into one `volatile unsigned char` bitmask, so the added arms
cost ZERO net static RAM -- which is not cosmetic: see the `bluepill-c8-st` zero-slack note
under *Program-memory floors*, where any growth in a shared test breaks a link no gate
covers.

It reports PARTIAL where no FREE DEV window can be minted -- on the host sim, because the
one window the sim admits is the fake register block that `periph_reg_write_mask` holds.
**That PARTIAL is invisible to every gate** -- it emits a `# diag` comment plus a plain `ok`
-- so a DEV-window-encodability regression on ARM would make every board take the PARTIAL
early return and lose the seam's whole refusal contract fleet-wide with CI still green.
Measured: the held arms DO run on `qemu`/`m3`/`m7`/`m33`/`riscv-mpu`/`microbit`; only `sim`
degrades, and there `periph_reg_write_mask` covers the same ground with the window it holds.

That second case, `periph_reg_write_mask` (sim only), is what puts the allowlist match, the
mask edge, refuse-not-trim and the containment refusal under a gate: it drives the SHARED
`KOS_SYS_PERIPH_REG_WRITE` dispatch arm and the same `caller_holds_mmio_reg`, with only the
table and the store target sim-side. Its containment arm requires `-KOS_EPERM` and not
`-KOS_EINVAL`, which is what proves the refusal came from the kernel's span check rather
than from the chip layer. It does NOT skip when it finds no window: a refusal there is a
regression in the grant path or a drift between its base list and the sim's, so it FAILS.

What no host gate answers is whether the bus DISCARDS an unprivileged direct store, which
is the whole premise of the seam. `user/apps/xmc4800-relax/pvprobe` is the discriminating
probe, running a direct store and a seam store to the same register from the same thread.
**That half ships gate-free** -- no QEMU model classifies a register's write side -- so the
XMC captures are its only evidence.

**That silicon arm is TAKEN** (`xmc4800-relax`, 2026-07-30, one run, one thread, one held
window): the three seam writes read back `exact`, while the same thread's direct stores to
the same `FDR`/`BRG`/`CCR` reported `DROPPED (post == pre)`. Both controls came out right
in the same capture -- `SCTR` (a `U,PV` register in the window) `LANDED` under a direct
unprivileged store, and an ungranted SCU poke faulted (`CFSR=0x82`,
`MMFAR=0x50004648`). Both refusals were taken on hardware too: off-allowlist `-22`
(`-KOS_EINVAL`) and unheld window `-1` (`-KOS_EPERM`). So the seam is not a no-op and not
a widening of the window.

---

## Minimum hardware requirement

KickOS has **two** floors and they are far apart. Conflating them is the trap this
section exists to prevent. Both are `-Os` figures, and the second one's SRAM half is a
**provisioning** statement, not a part size:

- the **run** floor -- kernel, console and a small app with a couple of threads:
  **32 KiB program memory + 16 KiB SRAM, at `-Os`**;
- the **suite** floor -- additionally host the fleet-uniform validation image
  (`selftest`, the TAP suite every board is witnessed with): **64 KiB program memory at
  `-Os`**, plus **16 KiB SRAM with a static-allocation profile** -- heap 0 and 1 KiB user
  stacks -- which buys a clean run of 63 `ok` / 0 `not ok`, 5 of those 63 being named
  skips. A **zero-skip** run needs a 32 KiB part.

`f302nucleo` (STM32F302R8, 64 KiB flash / 16 KiB SRAM) is the worked case for both, and
it is what makes the SRAM half a provisioning statement. At the chip defaults it refuses
every spawn. At the `f302nucleo-st` provisioning the same 16 KiB part runs the suite on
silicon at **63 ok / 0 not ok / 5 skipped**, plan `1..63` (measured at `124b68c`,
`.session/m456-silicon/b5-nuc-selftest-after.log`). That provisioning is split across two
files and a porter must read both: the PRESET (`cmake/presets/arm.json`, `f302nucleo-st`)
sets `KICKOS_ENABLE_SELFTEST=ON`, `KICKOS_USER_HEAP_SIZE=0`, `KICKOS_MAX_SEMAPHORES=6` and
`KICKOS_MAX_THREADS=3`, while the stack sizes live in the chip's `board_config.h`
(`KICKOS_USER_STACK_SIZE 1024`, `KICKOS_ROOT_STACK_SIZE 1536`, `KICKOS_IDLE_STACK_SIZE
512`). So "KickOS runs here", "KickOS is validated here with named skips" and "KickOS is
validated here with none" are three different claims about one board.

A **named** reduced suite is a supported posture, not a degraded one. `microbit` (nRF51,
16 KiB SRAM) declares the eleven cases it cannot host as an expected-skip list checked by
name in CI (`KICKOS_EXPECT_SKIPS` in `../../user/apps/common/selftest/CMakeLists.txt`,
rationale in `boards.md` under *Per-board caveats*): a skip not on that list fails the gate.
`f302nucleo` has no gate of any kind, so its 5 skips are enumerated below instead.

Every figure below names its board, its app and its optimisation level. Flash figures are
reused from `../design-flash-footprint.md` section 3; RAM figures are read out of ELFs
linked at this branch tip.

### The optimisation level is part of every floor

Every byte count in this section is `-Os`. The fleet gets that from `CMAKE_BUILD_TYPE`
`MinSizeRel` in `cmake/presets/arm.json:10`, which is **in-tree only**: an out-of-tree
consumer reaching KickOS through `find_package(KickOS)` picks its own `CMAKE_BUILD_TYPE`
and never sees the presets. Both the empty default and `Debug` are `-O0`.

`f302nucleo` `hello`, same tree, same tip, flash = `.text` + `.data` + `.init_array` +
`.ARM.exidx`:

| Build type | flash |
| --- | --- |
| `MinSizeRel` (`-Os -DNDEBUG`) | 19,088 |
| `Debug` (`-O0 -g`) | 36,852 |

A floor quoted bare is therefore off by 1.9x: "fits in 32 KiB" is true at `-Os` and false
at `-O0`. Quote the optimisation level with the number. The installed package does
**not** override a consumer's build type -- that is the consumer's decision -- so the
condition travels as documentation and nothing else.

**`Debug` is not a supported configuration on a 64 KiB part.** The suite floor is an
`-Os` floor and the `-O0` image does not fit. Measured at `c5d9b0d`, and in both cases
`selftest` is the ONLY image that fails to link -- every other app on the board fits even
at `-O0`:

```
ld: user/apps/common/selftest/selftest section `.text' will not fit in region `FLASH'
ld: region `FLASH' overflowed by 7920 bytes      # f302nucleo-st
ld: region `FLASH' overflowed by 7692 bytes      # bluepill-c8-st
```

The same `f302nucleo-st` `selftest` at `-Os` uses 54,944 B of its 64 KiB and keeps
**10,592 B (10.34 KiB) free**. **All five of those figures were measured at `c5d9b0d` and
are NOT current** -- the tree is at `124b68c` and two further commits have grown the suite,
so treat them as the shape of the result, never as today's numbers. They move with every
milestone that grows the suite: they read 5,120 / 4,884 / 12.9 KiB one milestone earlier,
before two syscalls and three selftest arms landed. Re-measure rather than trusting any
quoted figure, this one included. This costs no
debuggability: `MinSizeRel` carries `-g`, so the symbols are all there and only `-O0` is
unavailable. No gate builds these boards in `Debug`, deliberately -- the link failure
already names the overflow in bytes.

**`bluepill-c8-st` has exactly ZERO boot-arena slack**, which makes it the fleet's most
fragile link and the one nothing will catch. Configure prints only the needed side:

```
-- KickOS: boot stacks idle=512->512/16 root=2048->2048/16 (mpu granule 0 pow2=1)
```

512 + 2048 = **2,560 B needed** against **2,560 B available**
(`__kickos_ram_end - __kickos_ram_start` = `0x20004800 - 0x20003e00`). The
`KICKOS_BOOT_ARENA_ASSERT` is `<=`, so the exact fit passes and one byte of static-RAM
growth in a SHARED test breaks the link. It is the ONLY image in the 921-image fleet at or
below zero (measured at `124b68c`); the next tightest are `bluepill-c8` / `selftest` at
96 B and `f302nucleo` / `selftest` at 2,496 B. There is no available-vs-needed pair anywhere in
the build output and the `ASSERT` message carries no numbers, so the failure is loud but
mute. The board has **no ctest gate** (`ctest -N` in its build dir lists only the
universal `kickos_build` fixture) and **no physical unit**, so neither CI nor a bench run
can catch it -- only a full-fleet build. A regression on this branch was exactly that: an
arena starvation fixed at the source rather than by raising a limit.

### The program-memory floor

`hello` -- kernel, arch, chip, console, UART driver, libgcc, minimal app -- across the
fourteen real boards (`../design-flash-footprint.md` s.3):

| | Board | `hello` flash, `-Os` |
| --- | --- | --- |
| minimum | `microbit` (nRF51, armv6m) | 18,708 |
| ARM maximum | `teensy41` (imxrt1062, M7) | 20,692 |
| RX | `rx72m` | 21,732 |
| Xtensa | `esp32-wroom` | 22,996 |

So the kernel plus a small app costs **18.7 to 23.0 KiB at `-Os`**, of which the kernel core is
a flat ~12.5 KiB across boards (`../design-flash-footprint.md` s.4). A 32 KiB part holds
that with room for an app, but **no chip in the tree declares one**: the smallest FLASH
regions are 64K, on `stm32f302` (`arch/arm/chip/stm32f302/stm32f302.ld:13`) and
`stm32f103` (`arch/arm/chip/stm32f103/stm32f103.ld:17`). The 32 KiB run floor is derived
from the measured `hello` sizes, not witnessed on a part.

The two non-ARM ISAs cost 1.0 to 2.3 KiB more than the ARM maximum. `esp32c6-wroom`
looks far larger -- 49,112 bytes in `../design-flash-footprint.md` s.3 -- but that is
**region accounting, not code**: its linker script carves no flash region, so code and
data share the 512 KiB RAM and the figure is whole-RAM occupancy. Measured at tip, its
`hello` code is ordinary: `.text` 22,840 + `.data` 48 + `.init_array` 8 = 22,896, and
the rest of the 49,072-byte total is `.bss` 9,792 plus a 16,384-byte `.userheap`
(`arch/riscv/chip/esp32c6/esp32c6.ld:61`). Per-ISA code density accounts for the
remaining ARM/non-ARM gap; that attribution is **inferred, not measured**.

The suite spans 46,932 to 57,568 bytes at `-Os` across the fleet and both
`KICKOS_ENABLE_SELFTEST` settings (`../design-flash-footprint.md` s.3), so it needs a
**64 KiB** part; the tightest shipped configuration keeps 13,820 bytes free.

### The SRAM model

SRAM divides into four spans and a porter must budget all four. Every boundary is a
linker-script symbol, so the split is readable out of any linked ELF:

    SRAM = static + .userheap + arena + _kernel_stack_size

- **static** -- `.data` plus real `.bss`, dominated by `kickos::detail::g_instance`,
  the kernel singleton holding every object pool
  (`kernel/include/kickos/instance.h:65-99`).
- **`.userheap`** -- `KICKOS_USER_HEAP_SIZE`, carved *below* `__kickos_ram_start`
  (`arch/arm/chip/stm32f302/stm32f302.ld:105`), so it trades against the arena 1:1.
  Per-chip default, `-D`-overridable (`arch/CMakeLists.txt:312-314`).
- **arena** -- `[__kickos_ram_start, __kickos_ram_end)`
  (`arch/arm/chip/stm32f302/stm32f302.ld:110-111`), the MPU-governed user-RAM pool.
  **Every thread stack comes from here**, as does every `kos_ram_alloc`.
- **`_kernel_stack_size`** -- the MSP/boot stack, taken off the top
  (`arch/arm/chip/stm32f302/stm32f302.ld:18`, 2K there; `__kickos_ram_end = _estack -
  _kernel_stack_size`).

`arm-none-eabi-size` reports `.userheap` inside its `bss` column (the section is
`ALLOC`/`NOBITS`), so that column is **not** the static footprint.

The arena must hold, allocated in this order (`kernel/init/kmain.cc:222`, `:224`):

    arena >= align(KICKOS_IDLE_STACK_SIZE)
           + align(KICKOS_ROOT_STACK_SIZE)
           + N * align(KICKOS_USER_STACK_SIZE)
           + whatever the app kos_ram_alloc's

for N **concurrently live** spawned threads taking a kernel-default stack
(`kernel/syscall/syscall_thread.cc:354`). `align()` is `arch_ram_region_size` /
`arch_ram_region_align` (`arch/include/kickos/arch/arch.h`), which have **three** modes,
keyed on `arch_mpu_min_region()` and `arch_mpu_region_pow2()`:

| `min` | `pow2()` | size | align | backends |
|---|---|---|---|---|
| 0 | n/a | 16-byte granular | 16 | the three no-MPU chips (`nrf51/chip_nrf51.cc:110`, `stm32f103/chip_stm32f103.cc:383`, `stm32f302/chip_stm32f302.cc:321`) and LX6 |
| != 0 | 1 | power of two, >= `min` | the size | ARM PMSAv7 (32), RISC-V PMP NAPOT (8) |
| != 0 | 0 | multiple of `min` | `min` | ARM PMSAv8 (32), NXP SYSMPU (32), RX (16) |

Only the pow2 mode pays a natural-alignment run-up, and there it can cost as much again
as the request, so compute it rather than assuming the sum of the sizes. A base+limit
backend pays at most one granule per block.

On a v8-M chip the mode is **posture-dependent**: `arch_arm_pmsav8.cc` (which defines
`pow2() == 0`) enters the link only at `KICKOS_HAVE_MPU=1`, so a non-enforcement build
of the same chip shapes regions with the v7-M pow2 fallback.

The arena is a pure bump allocator (`arch/common/arch_ram_common.cc:42`) and **never
takes a block back**. An exited thread's default stack returns to a single-size-class
free list on the thread pool instead (`kernel/include/kickos/thread.h:203-207`), which
is why N is a peak-concurrency figure while every `kos_ram_alloc` is permanent.

**The idle + root terms are checked at link time on every board, and the thread-stack POOL
where a chip opts in.** `KICKOS_BOOT_ARENA_ASSERT` (`arch/common/boot_arena.ld.h`, fed by
`cmake/boot_arena.cmake`) replays those two allocations including alignment padding, and no
chip may opt out by omission: a `<chip>.ld` carrying no invocation is a configure
`FATAL_ERROR`. `KICKOS_POOL_ARENA_ASSERT` (same header) carries the same replay one step
further, over `KICKOS_MAX_THREADS` blocks of `align(KICKOS_USER_STACK_SIZE)` -- but it is
**OPT-IN per chip `.ld`**, and exactly ONE script invokes it today,
`arch/arm/chip/stm32f302/stm32f302.ld`, so it covers `f302nucleo` and `f302nucleo-st` only.
Configure prints the demand terms the assert replays, and only the demand side:

```
-- KickOS: arena model idle=<size>/<align> root=<size>/<align> pool=<count>x<size>/<align>
```

Where a board does NOT opt in, an image whose arena cannot back every advertised slot's
stack links clean and fails per-spawn at runtime with `-KOS_ENOMEM` -- the same code as a
full slot table, so the shortfall is indistinguishable from a legitimate limit. That
asymmetry is why the two floors are indistinguishable from the build system and obvious on
silicon.

**Why the pool assert is not fleet-wide.** `frdmk64f` (at `KICKOS_HAVE_MPU=1`),
`bluepill-c8` and `bluepill-c8-st` still advertise more slots than their arena backs, so a
fleet-wide assert would break those links today. Worst image per config, measured at
`124b68c`: `frdmk64f-st +MPU` **-28,992 B**, `frdmk64f +MPU` -28,960 B, `bluepill-c8-st`
-4,096 B, `bluepill-c8` -4,000 B.

**Headroom is a LINK-TIME, PER-IMAGE quantity -- not per-preset and not per-board.** Each
app's static footprint moves the arena base, and `bluepill-c8-st` alone spans -4,096 to
-96 B across its images. So a porter must never quote one headroom number for a board; the
figure is per image or it is meaningless. Only the linker knows the arena base, which is
exactly why the assert lives in the linker script and not in CMake.

One mechanism worth recording, because it names the real culprit on the worst board:
`frdmk64f` WITHOUT the MPU has **+81,856 B** of headroom. Turning `KICKOS_HAVE_MPU=1` on
moves `__kickos_ram_start` from `0x1fff78a0` to `0x20012940` -- the `.appdata` enforcement
window eats 110,748 B of arena. So it is ENFORCEMENT, not the pool, that makes that board
overcommitted.

The model is validated against the real linker rather than by inspection: `f302nucleo-st`
at `KICKOS_MAX_THREADS=3` predicts **+928 B** and links, and at `-DKICKOS_MAX_THREADS=4` it
predicts **-96 B** and the link FAILS with the `KICKOS_POOL_ARENA_ASSERT` message. The sign
flip lands where the model says it does.

### The heap is a per-board profile, not a requirement

`KICKOS_USER_HEAP_SIZE 0` is a supported profile, not a broken one. Newlib falls back to
unbuffered stdio when the stream-buffer `malloc` fails, so `printf` and `std::cout` still
emit with no heap (`arch/arm/chip/stm32f302/stm32f302.ld:24-26`). The in-tree precedent is
`nrf51`, which ships heap 0 (`arch/arm/chip/nrf51/nrf51.ld:29`) and whose
`microbit_selftest` QEMU gate is green
(`../../user/apps/common/selftest/CMakeLists.txt:96`).

A heapless profile costs stdio **buffering**, and `malloc` for an app that wants it. It
does not cost the standard-API surface. The project's rule that user-facing apps are
written against `printf`/`std::cout` rather than `kos_*` requires those APIs to work, not
a heap.

So the SRAM floor is **16 KiB with a static-allocation profile**, and the carve is a
per-board decision recorded in the chip's linker script: 0 on `nrf51`, 2K on `stm32f302`
(`arch/arm/chip/stm32f302/stm32f302.ld:28`), 8K on `stm32f103`
(`arch/arm/chip/stm32f103/stm32f103.ld:27`), 16K on `esp32c6`
(`arch/riscv/chip/esp32c6/esp32c6.ld:61`).

A part at the floor **plus MPU enforcement** is tighter still, because enforcement costs
RAM of its own: region descriptors, per-domain data in `g_instance`
(`kernel/include/kickos/instance.h:95`), and a fixed `.appdata` window carved for app
globals as one aligned region. The smallest window in the tree is 16 KiB
(`arch/arm/chip/stm32f411/stm32f411.ld:31`) -- the entire SRAM of a part at the floor. No
board in the tree is both small and enforcing: the smallest-SRAM MPU/PMP chips are
`stm32f411` and `xmc4800` at **128 KiB**, and every other one is larger.

### Worked arithmetic

Measured at tip, `-Os`. The four spans sum to the part's SRAM exactly; that identity is
the first check a porter should run.

| Board | SRAM | App | static | `.userheap` | arena | boot stack |
| --- | --- | --- | --- | --- | --- | --- |
| `f302nucleo` | 16,384 | `hello` | 3,776 | 2,048 | **8,512** | 2,048 |
| `f302nucleo` | 16,384 | `selftest`, chip defaults | 7,760 | 2,064 | **4,512** | 2,048 |
| `f302nucleo` | 16,384 | `selftest`, `f302nucleo-st` | 8,672 | 0 | **5,664** | 2,048 |
| `bluepill-c8` | 20,480 | `hello` | 3,656 | 8,216 | **6,560** | 2,048 |
| `bluepill-c8` | 20,480 | `selftest` | 7,640 | 8,200 | **2,592** | 2,048 |
| `microbit` | 16,384 | `hello` | 2,840 | 8 | **11,488** | 2,048 |
| `microbit` | 16,384 | `selftest` | 6,824 | 24 | **7,488** | 2,048 |
| `f411disco` | 131,072 | `hello` | 6,872 | 8,200 | **107,808** | 8,192 |
| `f411disco` | 131,072 | `selftest` | 10,760 | 8,216 | **103,904** | 8,192 |

Thread capacity that follows, each board with its own stack sizes:

| Board / app | idle | root | user | arena less boot stacks | N by arena | `MAX_THREADS` | N |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `f302nucleo` `hello` | 512 | 2,048 | 2,048 | 5,952 | 2 | 2 | **2** |
| `f302nucleo` `selftest`, chip defaults | 512 | 2,048 | 2,048 | 1,952 | **0** | 2 | **0** |
| `f302nucleo` `selftest`, `f302nucleo-st` (pre-`124b68c`) | 512 | 2,048 | 1,024 | 3,104 | 3 | 4 | **3** |
| `bluepill-c8` `hello` | 512 | 2,048 | 2,048 | 4,000 | **1** | 2 | **1** |
| `bluepill-c8` `selftest` | 512 | 2,048 | 2,048 | 32 | **0** | 2 | **0** |
| `microbit` `hello` | 512 | 2,048 | 2,048 | 8,928 | 4 | 2 | **2** |
| `f411disco` `hello` | 2,048 | 4,096 | 4,096 | 101,664 | 24 | 8 | **8** |

The `f302nucleo-st` row and the two tables above it predate the `124b68c` right-size, which
took `KICKOS_ROOT_STACK_SIZE` to 1,536 and `KICKOS_MAX_THREADS` to 3; N stayed **3**, so the
reading holds while the byte columns do not. Re-link and re-read them rather than quoting
them (step 5 of the checklist below is that measurement).

Four readings, and they are the point of the section:

- **`f302nucleo` `hello` fits with nothing spare.** It needs exactly two threads
  (`../../user/apps/common/hello/main.cc:74-75`) and gets exactly two, from both
  constraints at once. Silicon-witnessed.
- **`f302nucleo` `selftest` is provisioning-bound, not part-bound.** At the chip defaults
  it gets zero threads, and on silicon the suite failed every spawning case (17 ok /
  42 not ok / 10 skipped) on `w >= 0` / `drv >= 0` assertions -- a negative spawn return,
  which is what N=0 predicts. Raising `KICKOS_MAX_THREADS` to 4 alone moved the tally not
  at all: the arena bound, not the pool. Surrendering the 2K heap carve and halving
  `KICKOS_USER_STACK_SIZE` to 1,024 takes the arena to 5,664 and N to 3, and the same
  part then runs the suite at 63 ok / 0 not ok / 5 skipped. Silicon-witnessed both ways.
- **SRAM size is not the ranking.** `bluepill-c8` has 4 KiB *more* SRAM than
  `f302nucleo` and hosts *fewer* threads, missing `hello`'s second stack by 96 bytes,
  because its heap carve is 8K against f302's 2K
  (`arch/arm/chip/stm32f103/stm32f103.ld:27`,
  `arch/arm/chip/stm32f302/stm32f302.ld:28`). `microbit`, the same 16 KiB part class,
  carves heap 0 (`arch/arm/chip/nrf51/nrf51.ld:29`) and has the roomiest small-board
  arena in the fleet. **The heap carve, not the part's SRAM, is usually what binds.**
  (`bluepill-c8` is build-only, so its N=1 is a model prediction, not a witness.)
- On a comfortable board the constraint **inverts**: `f411disco`'s arena would hold 24
  user stacks and `KICKOS_MAX_THREADS 8` is what caps it
  (`../../boards/f411disco/include/kickos/board_config.h:17`). Above roughly 32 KiB of
  SRAM, provisioning is the knob and the part is not the limit.

### What binds beyond memory

The arena is one of two independent constraints on a spawn; the other is object-pool
capacity, all of it inside `g_instance` and all `-D`-overridable. **Only the thread pool is
checked at link time, and only where a chip opts in** -- `KICKOS_POOL_ARENA_ASSERT`, see
*The SRAM model*. Every other pool below is unchecked at build time. The suite's own
requirement, for a zero-skip run:

| Knob | Default | Pool | Suite needs | Tight-board value |
| --- | --- | --- | --- | --- |
| `KICKOS_MAX_THREADS` | 16 (`config/system.h:42`) | `thread.h:199` | **>= 4** | 2 |
| `KICKOS_MAX_SEMAPHORES` | 16 (`system.h:24`) | `instance.h:65` | **>= 6** | 4 (f302) |
| `KICKOS_MAX_HANDLES` | 12 (`system.h:55`) | per-thread cap table | **>= 9** | 9 |
| `KICKOS_MAX_MUTEXES` | 8 (`system.h:30`) | `instance.h:75` | >= 2 | 4 (c8) |
| `KICKOS_MAX_ENDPOINTS` | 4 (`system.h:37`) | `instance.h:83` | >= 1 | 4 |
| `KICKOS_MAX_IRQ_HANDLES` | 8 (`system.h:99`) | `instance.h:99` | >= 1 | 4 (f302) |
| `KICKOS_MAX_DOMAINS` | `MAX_THREADS + 2` (`system.h:61`) | `instance.h:95` | derived | 4 |

The `Tight-board value` column is the chip default. The `f302nucleo-st` PRESET
(`cmake/presets/arm.json`) overrides semaphores to 6 and threads to **3** -- not 4: it was
cut at `124b68c` once the pool assert made the overcommit a link error, and any text citing
`arm.json` for `KICKOS_MAX_THREADS=4` on this board is stale. The stack sizes moved the
other way, out of the preset and into the chip's `board_config.h`
(`KICKOS_USER_STACK_SIZE` 2048 -> **1024**, `KICKOS_ROOT_STACK_SIZE` 2048 -> **1536**),
chosen against MEASURED paint-and-scan watermarks now recorded beside them in that header:
deepest pool worker 592 B, root 1,048 B, idle 76 B -- a 488 B (31.8%) margin on the 1,536 B
root stack. That is the pattern to copy on a tight part: measure the watermark, then
provision, rather than provisioning for comfort.

Only `KICKOS_MAX_HANDLES >= 9` is asserted in-tree (`system.h:49-53`); the rest are
measured off the suite's own call sites. **Two** of the 63 cases need a 4th concurrent
worker: `call_infoless_revert`, four mutually-dependent workers spawned before any join
(`../../user/apps/common/selftest/main.cc:2212-2215`), and `mutex_chain_boost`, a four-link
boost chain (`main.cc:1010-1013`). `call_infoless_revert` is the only case that asks first
-- `pool_can_host(4)` at `:2199` is the file's **only** pool-capacity guard, and since it
spawns four real threads it tests the arena as much as the pool; `mutex_chain_boost`
detects the same shortfall from a negative spawn return (`:1014`) -- and a negative spawn
return CANNOT say which of the two limits it hit, because `kos_thread_spawn` answers
`-KOS_ENOMEM` for a full slot table and for a missing stack block alike. The other 61 cases
need no 4th worker, so a small part loses one chained-priority-inheritance case and
one call/reply case, not the suite. The 6-semaphore peak is `mutex_deadlock`: two permanent
plus four live (`main.cc:1126-1129`) -- but on a 9-handle board the **cap table** binds
first, since `KICKOS_CAP_FIRST_DYNAMIC 4`
(`../../system/include/kickos/sys/cap_index.h:22`) plus the suite's two permanent caps
leaves 3 free against the 6 that case wants live (rationale `main.cc:1133-1136`).

`f302nucleo` walked that ladder one knob at a time: at `KICKOS_MAX_THREADS=4` plus
`KICKOS_USER_STACK_SIZE=1024` and the 2K heap still carved, which bought one more case
(18 ok / 41 not ok / 11 skipped), `sem_destroy` then failed on a semaphore **handle**
against `KICKOS_MAX_SEMAPHORES 4` (`main.cc:685`); adding `KICKOS_USER_HEAP_SIZE=0` and
`KICKOS_MAX_SEMAPHORES=6` is what took the run to 0 `not ok`. **A spawn failure is not
evidence that the thread pool is the limit; check the arena first, and expect the next pool
behind it.** The runtime cannot tell you which one you hit -- `-KOS_ENOMEM` covers both --
which is why the honest place to answer it is the link, and why `KICKOS_POOL_ARENA_ASSERT`
exists. Eight of `f302nucleo`'s nine pre-`124b68c` skips were arena starvation labelled
"pool too small"; see *The 5 skips on a 16 KiB part*.

`KICKOS_MAX_THREADS` counts **spawnable** threads only. Idle and root run on
file-static TCBs (`kernel/init/kmain.cc:85-86`) handed to `thread_create` by pointer,
so they take no pool slot -- which is why `f302nucleo` runs a two-thread app at
`KICKOS_MAX_THREADS 2`. The knob dominates static RAM: on `f302nucleo` `selftest` at
`KICKOS_MAX_HANDLES=9` and heap 0, `g_instance` measures 2,448 bytes at 2 threads,
3,296 at 4, 4,152 at 6 and 5,000 at 8 -- **about 424 bytes per slot**, because each slot
buys a `Domain` too (`system.h:61`).

The suite also allocates from the arena and never returns it: one 4 KiB page
(`main.cc:504`) and three 256-byte domain regions (`:1505`, `:2901-2902`), 4,864 bytes in
all, plus the two self-grant probe ladders (`:3221`, `:3283`) and one spare stack for
`caller_stack` (`:1443`). Each has a real `tap::skip` or a `PARTIAL` diag when
the arena cannot spare it, so these cost coverage rather than a failure.

### Deriving the suite's SRAM figures

Measured, not assumed -- but measured BEFORE the `124b68c` right-size, which took
`KICKOS_ROOT_STACK_SIZE` to 1,536 and `KICKOS_MAX_THREADS` to 3. Every byte column below is
therefore the older link; the METHOD is what to reuse, and step 5 of the checklist is how.
`f302nucleo` `selftest` at the then-shipped `f302nucleo-st` provisioning
(`cmake/presets/arm.json`, `f302nucleo-st`; `KICKOS_MAX_HANDLES` stays at the chip's 9,
`arch/arm/chip/stm32f302/include/kickos/board_config.h`), `-Os`, no-MPU 16-byte
granule. `g_instance` measures 3,336:

    static  (.data 380 + .bss 8,260 + 32 alignment)   8,672
    .userheap  (KICKOS_USER_HEAP_SIZE=0)                  0
    arena   [__kickos_ram_start, __kickos_ram_end)    5,664
    _kernel_stack_size                                2,048
    ------------------------------------------------------
    SRAM                                             16,384

That arena pays `align(512) + align(2,048) = 2,560` for the boot stacks and then holds
`floor((5,664 - 2,560) / 1,024) = 3` user stacks with 32 bytes to spare, so
N = min(3, 4) = **3**. That is the whole result and the right-size did not move it: **the
suite passes on 16 KiB of SRAM**, 63 ok / 0 not ok / 5 skipped, because the cases that do
not need a 4th worker all run at N = 3. This arithmetic is exactly what
`KICKOS_POOL_ARENA_ASSERT` now replays at link time for this chip, with the run-ups paid
against the real arena base instead of a quoted one.

A **zero-skip** run is what needs a bigger part. On top of the 16,384 above it wants a 4th
user stack (1,024) and the permanent arena allocations the remaining skips decline -- the
4 KiB MMIO page and the `kos_ram_alloc(1)` ladder -- plus, at the earlier provisioning, the
shared domain region and the two cross-domain buffers those four un-skipped cases now do
take. That is **a 32 KiB part**, and an enforcing chip pays an `.appdata` window
besides, plus power-of-two natural alignment on every block where
`arch_mpu_region_pow2()` is 1 (a base+limit backend pays only one granule per block). One of
the five is not a RAM question at all: `mutex_deadlock` wants `KICKOS_MAX_HANDLES` above 9.
`f411disco` is the zero-skip
witness -- 128 KiB SRAM, 0 skips -- and it provisions `KICKOS_MAX_THREADS 8`
(`../../boards/f411disco/include/kickos/board_config.h:17`), above the measured 4-thread
peak, so 4 is a floor and not a fitted value.

The 16 KiB result is not slack to spend elsewhere: raising only the thread pool on this
part fails the **link** at `KICKOS_MAX_THREADS=12`, with `KICKOS_BOOT_ARENA_ASSERT`
reporting that the arena can no longer hold idle + root.

### The 5 skips on a 16 KiB part

`f302nucleo` was re-provisioned at `124b68c` and the measured skip count on silicon went
**9 -> 5**. Silicon, `.session/m456-silicon/b5-nuc-selftest-after.log`: plan `1..63`,
`# skipped: 5`, `# all tests passed (5 skipped)`. The BEFORE run (`b2-nuc-selftest.log`)
was `# skipped: 9`, and it also carried a real failure -- `not ok 46 -
periph_enable_unheld`.

Every skip is a **named** resource decline the case emits about itself, not a silent pass.
Four are the concurrent-worker ceiling or the arena; one is the cap table:

| Case | TAP skip reason | What actually binds | Site |
| --- | --- | --- | --- |
| `mutex_chain_boost` | pool too small | 4th worker against N = 3 | `main.cc` (`t_mutex_chain`) |
| `call_infoless_revert` | pool too small (4 interdependent workers) | 4th worker against N = 3 | `main.cc` (`t_call_infoless_revert`) |
| `mutex_deadlock` | pool too small | 6 live caps against 3 free at `KICKOS_MAX_HANDLES 9` | `main.cc` (`t_mutex_deadlock`) |
| `irq_as_event` | 4 KiB MMIO-page alloc failed -- board too small | one 4,096 B block | `main.cc` (`t_irqdrv`) |
| `mem_self_grant` | arena too small to reach the region ceiling | the `kos_ram_alloc(1)` ladder | `main.cc` (`t_selfgrant`) |

The FOUR that un-skipped are `endpoint_crossdomain`, `mem_self_grant_nonpow2`,
`region_mode` and `domain_share` -- all four wanted arena, and arena is what the
re-provisioning bought. `caller_stack` still reports `PARTIAL` as a `tap::diag`
(`t_caller_stack`: `# caller_stack: PARTIAL -- accept half not run (arena cannot spare a
stack)`) and counts as a pass, not a skip; `confused_deputy` carries the same construct and
did not trip it on this part.

**8 of the 9 former skips were arena STARVATION wearing a "pool too small" label.** That
label is not sloppiness in the cases: `kos_thread_spawn` returns `-KOS_ENOMEM` for BOTH
"slot table full" and "no stack block", so the two are INDISTINGUISHABLE at runtime and a
case that only sees a negative spawn return cannot tell which limit it hit. This is the
same trap *What binds beyond memory* states from the other end -- a spawn failure is not
evidence that the thread pool is the limit -- and it is why the pool term is now checked at
LINK time on boards that opt in (see *The SRAM model*). At the current
`KICKOS_MAX_THREADS 3` against an arena that backs exactly three stacks, the two remaining
4th-worker skips are honest under either reading.

**`mutex_deadlock` is mislabelled differently, and no arena work will ever un-skip it**:
its guard is `KICKOS_MAX_HANDLES = 9` / semaphore exhaustion, not memory at all.

The genuine 16 KiB limits that remain are small and specific: `irq_as_event` needs one
4,096 B block, and `caller_stack`'s accept half needs 2,064 B.

### Porter's checklist

Given a new part's flash and SRAM, in order:

1. **Program memory, and the optimisation level with it.** Below ~24 KiB at `-Os`, stop:
   the kernel plus a minimal app does not fit -- and the same image is 1.9x larger at
   `-O0`, so budget roughly double if the consumer does not set `CMAKE_BUILD_TYPE`.
   Below 64 KiB the part runs KickOS but cannot host the validation suite -- plan to
   witness it with `apps/blink` and `hello`, or with a named expected-skip list as
   `microbit` does.
2. **Write the board's stack and pool knobs explicitly** in `board_config.h`. There is
   no useful default on a small part: the `config/system.h` fallbacks are 64 KiB stacks
   and 16 threads, sized for the host sim. If the board ships its own header it must
   restate EVERY knob it needs -- `boards/<board>/include` replaces the chip's header
   outright and does not layer over it.
3. **Decide the heap carve first, not last.** `KICKOS_USER_HEAP_SIZE` comes out of the
   arena 1:1, and on a 16-20 KiB part it is routinely the difference between one thread
   and four. Zero is a supported profile: it costs stdio buffering and `malloc`, not
   `printf`/`std::cout` -- see *The heap is a per-board profile* above.
4. **Read `arch_mpu_min_region()` AND `arch_mpu_region_pow2()` for your chip** before
   computing anything. Only a nonzero granule with `pow2() == 1` makes every arena block
   power-of-two and naturally aligned, which can double the cost of a stack; a base+limit
   backend rounds to the granule instead. See the table above.
5. **Link `hello` and read the four spans** out of the ELF -- `_estack`,
   `_kernel_stack_size`, `_kickos_heap_start`, `__kickos_ram_start`,
   `__kickos_ram_end`. They must sum to the part's SRAM. This is the measurement, and it
   costs one build.
6. **Compute N** = `floor((arena - align(idle) - align(root)) / align(user))`, then
   `N = min(N, KICKOS_MAX_THREADS)`. That is how many threads the board will host.
   Subtract any `kos_ram_alloc` the app makes first.
7. **If N is short**, in decreasing order of yield: cut `KICKOS_USER_HEAP_SIZE`, cut
   `KICKOS_USER_STACK_SIZE`, cut `KICKOS_MAX_THREADS` (~424 bytes of static RAM each),
   then the other pools. Do **not** raise `KICKOS_MAX_THREADS` to fix a spawn failure
   without checking the arena: on a tight part the arena is usually the binding term,
   and the raise spends static RAM to make the arena smaller.
8. **A clean link proves that the boot stacks fit, and the thread-stack pool only if your
   `<chip>.ld` opted in** to `KICKOS_POOL_ARENA_ASSERT`. Opt in: the alternative is a
   board that advertises slots it cannot seat, and `-KOS_ENOMEM` will not tell you which
   limit you hit. Nothing checks the other pools at build time, so an object create
   returning a negative handle is still a hardware-first sign.
9. **If your `<chip>.ld` writes an `AT` clause, decide whether the LOADER honours LMA**
   before writing it, and pin the decision with an `ASSERT` either way -- see *An `AT`
   clause in `<chip>.ld` is only valid if the LOADER honours LMA* above. An unhonoured LMA
   is a silent `.data` corruption whose symptom varies by die and does not bisect.
10. **Put every seam your chip defines in `chip_<chip>.cc`** -- or in another member the link
    always pulls for some other reason -- and NEVER in a dedicated TU nothing else references.
    A seam's fallback is a one-symbol archive member, not a weak symbol, so an unanchored
    definition is never extracted: the fallback answers first and the board silently DECLINES
    at runtime. Gated by ctest `seam_defaults`; see *Privileged register write* above.

---

## The ARMv7-M syscall spike (the M1 de-risk)

### The problem

`arch.h` requires (portability-critical contract, quoted from the header):

> the arch MUST run `syscall_dispatch()` in privileged **THREAD** context on the
> calling thread's own continuation -- NOT in ISR/handler context. A blocking
> syscall blocks by an ordinary synchronous context switch ... `arch_in_isr()` must
> read false during dispatch.

On Cortex-M, `SVC` traps into **handler mode**. If `syscall_dispatch()` ran in the
SVC handler, then during a blocking call:

- `arch_in_isr()` (IPSR != 0) would read **true** -- contract violation;
- a blocking `arch_switch()` pends **PendSV**, but PendSV is lower priority than
  the active SVC handler, so it **cannot preempt** it. The switch would be
  deferred until SVC returns -- i.e. the syscall would return to the user instead
  of blocking. The kernel's blocking primitives would break.

The roadmap note: *validate this in M1 week one; if infeasible the fallback
(deferred syscall completion) is a core restructure.* **It is feasible.** Design:

### The design (implemented in `arch/arm/armv7m/switch.S`)

Dispatch runs in **privileged thread mode** via an exception-return trampoline.
Two observations make it small:

1. The hardware exception frame that `SVC` stacks **already carries the syscall
   arguments** (`r0`=nr, `r1..r3`=a0..a2, `r12`=a3 -- the user stub loads a3 into
   r12 before `svc`) and, in the stacked `LR`, the return address of
   `arch_syscall`'s caller. So the SVC handler only has to rewrite the stacked
   `PC`.
2. **Lowering** privilege in thread mode needs no trap -- a plain `MSR CONTROL`
   with `nPRIV=1` suffices. Only *raising* privilege needs the trap.

Flow:

```
user (unprivileged, PSP)          arch_syscall:  ldr r12,[sp]  ; a3 -> stacked r12
                                                 svc #0
      |  hardware stacks {r0..r3,r12,lr,pc,xPSR} on PSP, enters handler mode
      v
SVC_Handler (handler mode)        rewrite stacked PC := svc_trampoline
                                  CONTROL.nPRIV = 0            ; trampoline privileged
                                  bx lr (EXC_RETURN thread/PSP)
      |  hardware unstacks -> thread mode, r0..r3/r12 = args, LR = caller-return
      v
svc_trampoline (PRIVILEGED THREAD mode, on the thread's own PSP stack)
                                  push {r12,lr}               ; a3 at [sp], save caller-return
                                  bl syscall_dispatch         ; <-- runs in thread mode
                                  ... (may block: see below) ...
                                  CONTROL.nPRIV = 1           ; drop to unprivileged
                                  bx lr                       ; -> arch_syscall's caller, r0=result
```

**Why blocking now works.** `syscall_dispatch` runs in *thread* mode, so
`arch_in_isr()` reads false. A blocking call reaches `arch_switch`, which pends
PendSV. When the kernel's `IrqLock` (BASEPRI) is released, **PendSV -- a real
exception, higher priority than thread mode -- preempts the trampoline** and
performs the switch. PendSV freezes the entire mid-dispatch continuation on
*this thread's PSP stack* and resumes it inline when the thread is next
scheduled. This is precisely the sim's synchronous-`swapcontext` semantics,
achieved with the native exception mechanism. No deferred-completion restructure
is needed. **Feasibility: confirmed.**

### Registers and privilege across a blocking switch

- The trampoline clobbers only `r0-r3,r12,lr`; the user's `r4-r11` (live since
  `svc`, untouched by exception entry/return and by the SVC handler) pass through
  to the caller intact. `syscall_dispatch` preserves `r4-r11` per AAPCS.
- Kernel syscall work runs on the **calling thread's stack** (as on the sim).
  User stacks must be sized for kernel call depth -- the M0 model; a separate
  per-thread kernel stack is a later refinement.
- **Privilege is a saved/restored register, not the resting privilege.** A thread
  blocked mid-syscall is *privileged* (the trampoline raised it). PendSV therefore
  **saves the outgoing thread's current `CONTROL.nPRIV`** into its context and
  restores the incoming thread's saved value -- so a mid-syscall thread resumes
  privileged, then the trampoline lowers privilege on the way back to the user.
  This is the ARM twin of the sim's `SimContext::raised` re-raise-on-switch-in.

#### Probing the ring: four design facts that are traps

All four are ARMv7-M architecture, not KickOS policy (ARM DDI 0403E.e). The prober that
pins them is `user/apps/common/ringpriv`.

- **A FAULT is the WRONG expectation for an unprivileged `MSR` to a privileged-only
  special register.** B5.2.3 gates the whole `MSR` group on `if CurrentModeIsPrivileged()`
  with no else clause, and its Exceptions clause reads "None". So an unprivileged write to
  `CONTROL.nPRIV` is **IGNORED, not trapped** -- a test asserting a fault would FAIL on
  correct hardware. The assertion has to be a READ-BACK.
- **Only `CONTROL` can carry that read-back.** B5.2.2's `MRS` pseudocode returns ZERO for
  `PRIMASK`/`BASEPRI`/`FAULTMASK` when unprivileged
  (`R[d]<7:0> = if CurrentModeIsPrivileged() then BASEPRI<7:0> else '00000000'`), so
  "wrote a mask, read back zero" is VACUOUS: it reads zero whether or not the write
  landed. `MRS CONTROL` carries no privilege guard in that same pseudocode. The obvious
  prober is the wrong one.
- **The read, the write, the `isb` and the read-back must be ONE asm block.** The syscall
  return path restores `CONTROL.nPRIV` from `ctx.resting_npriv`
  (`arch/arm/armv7m/switch.S`), so a single `print` between the `msr` and the read-back
  ERASES a real promotion and turns the gate GREEN on a broken system. Measured while
  building the prober: `0x2` before, `0x3` after a landed write, and back to `0x2` after
  one print. Capture first, print after -- this is the `npriv-banked-on-switch` invariant
  (`invariants.md`) seen from the userspace side.
- **Unprivileged PPB access is a DIFFERENT mechanism, and it is MPU-independent.** B3.1.1
  makes the PPB (`0xE0000000`-`0xE0100000`, which contains the System Control Space)
  privileged-access-only, so an unprivileged read of `SCB->CPUID` BusFaults with the MPU
  OFF. Confirmed on `f302nucleo` silicon by debugger attach: `CFSR=0x00008200` (BFSR
  `0x82` = `PRECISERR | BFARVALID`), `BFAR=0xe000ed00` (the exact probed address), and
  `HFSR=0x40000000` (`FORCED`, because `BUSFAULTENA` is never set in-tree). Assert `BFAR`,
  not a banner.

The gates are `tests/check_qemu_ringpriv.sh` and `tests/check_qemu_ringppb.sh`, and neither
is conditioned on enforcement: `cmake --preset qemu` IS the ring-only posture
(`KICKOS_HAVE_MPU` defaults to 0 off the sim), which is what makes the ring gateable in CI
rather than only capturable on no-MPU silicon. Both run permanently on the MPS2 M3/M4/M7/M33
arms. `microbit` (Cortex-M0: no Unprivileged/Privileged Extension, so `msr CONTROL` is
discarded) asserts the OPPOSITE outcome with one arm rather than skipping, which
machine-checks the armv6m classification instead of leaving it as prose; it does not build
`ringppb`, because on a no-ring core the PPB read legitimately succeeds and a confinement
gate there would assert the reverse. **A porting obligation follows for armv6m:** the core's
class is per BOARD, not per arch (M0+ implements the extension, M0 does not, and no
predefined macro separates them), so a new armv6m board must state its class in
`user/apps/common/ringpriv/CMakeLists.txt`. An unclassified one is a configure
`FATAL_ERROR` rather than a silently inherited default and a vacuous pass.

---

## Context switch (PendSV) & first-thread start

`arch_switch(from, to)` never switches inline: it records `to` in `g_arch_next`
and pends PendSV (the outgoing thread is always `g_arch_current`). PendSV:

- saves `{r4-r11, EXC_RETURN}` (and `{s16-s31}` iff the FP frame is active,
  keyed on `EXC_RETURN` bit 4) to the outgoing thread's PSP, stores its SP;
- loads the incoming thread's SP, restores the callee/FP registers, sets
  `CONTROL.nPRIV`, and exception-returns onto the incoming thread's PSP.

The saved-frame layout (low->high address on PSP):

```
[r4 r5 r6 r7 r8 r9 r10 r11 EXC_RETURN]   <- PendSV-saved   (ctx.sp points here)
[r0 r1 r2 r3 r12 LR PC xPSR]             <- hardware frame
```

`arch_context_init` fabricates exactly this: `r0`=arg, `PC`=entry, hardware-frame
`LR`=`kickos_thread_return` (a returning entry lands there), `EXC_RETURN`
=`0xFFFFFFFD` (thread/PSP, non-FP).

`arch_start` reuses PendSV: it sets `g_arch_current = 0` (the "nothing to save"
sentinel), points `g_arch_next` at the first thread, and pends PendSV. The switch
fires when `sched::start()` releases its `IrqLock`; the boot MSP frame is
abandoned (the system never returns to boot).

---

## Critical section, timer, clock, NVIC

- **Critical section** = `BASEPRI`. `arch_irq_save` raises BASEPRI to
  `PRIO_LOCK_BASEPRI` (0x20); PendSV/SysTick/SVCall sit at 0xE0-0xF0 and device
  IRQs must be configured >= 0x30, so the lock masks all of them while leaving a
  future 0x00/0x10 zero-latency band unmaskable. (v6-M/RP2040 will use PRIMASK.)
- **Monotonic clock** = the **DWT cycle counter**, extended to 64-bit in software.
  *Limitation:* a 32-bit wrap (~35 s at 120 MHz) not observed within one period
  is missed; a DWT/timer overflow interrupt is the refinement.
- **One-shot timer** = **SysTick** in a disarm-on-fire (tickless) mode. Deadlines
  beyond the 24-bit range fire early and the kernel re-arms the remainder (a
  harmless extra wake). A dedicated chip compare timer is the refinement.
- **NVIC** backs `arch_irq_mask/unmask/inject`. `inject` latches a raise on a
  masked (disabled) line (ISPR holds pending independent of ISER): it coalesces
  one-deep and fires at the next `unmask`. `arch_irq_clear_pending` (ICPR) is the
  explicit discard, used at first-arm to drop pre-registration garbage.
- **MPU** -- the shared ARMv7-M **PMSA** backend (`arch_arm_common.cc`) provides per-task
  enforcement at M2. `arch_mpu_apply` only **stashes** the incoming region set and is a PLAIN,
  non-overridable definition shared by every ARM backend; `kickos_arch_mpu_commit` (fallback TU
  `arch/arm/common/kickos_arch_mpu_commit_default.cc`) / `kickos_arm_mpu_program` **program the
  hardware** from the PendSV switch epilogue, after the physical swap (the deferred-commit seam,
  `design-mpu-commit-deferred.md`). A chip with a non-PMSAv7 MPU defines the commit, never
  `arch_mpu_apply` (K64F SYSMPU, RP2350 PMSAv8). `arch_mpu_region_encodable` bounds a grant to
  what the backend can describe.
- **Rule 7 reserved blocks (M4)** -- an enforcing chip MUST define `arch_reserved_blocks`
  (its owns-for-life peripherals: timebase, IRQ controller, every access-permission controller
  -- the MPU/PMP twin AND any bus-side gate, e.g. the K64F AIPS PACR pages or the ESP32-C6
  HP_APM/HP_TEE -- and the clock/reset gates); there
  is no fallback TU, so a missing one is a link error. A new **ARMv7-M chip with the
  Cortex-M bit-band alias (any M3/M4)** must also define `arch_bitband_present()` to return
  **1** -- the fallback answers 0, which is fail-OPEN for the bit-band alias refusal (a device
  grant into a reserved block's `0x42000000`/`0x22000000` alias image would be admitted).
  M7 (no bit-band) and non-ARM archs keep 0.
  On F411 it is **build + enforcement-link validated; silicon proof pending** (the
  canonical PMSA per-thread MMIO proof is `design-spi-driver-stm32f411.md`); PMSA
  enforcement is proven on silicon on XMC4800. See `m2-readiness.md`.

---

## The QEMU verification target (`boards`: `qemu`, chip `mps2`)

A runnable armv7m target validates the arch layer on real Cortex-M4:
`qemu-system-arm -M mps2-an386`. The `mps2` chip backend
(`arch/arm/chip/mps2/`) uses **semihosting** for the console (`SYS_WRITEC`) and
exit (`SYS_EXIT_EXTENDED`), so it needs no UART; its linker script maps code at
0x0 and SRAM at 0x20000000.

Two toolchain/QEMU gotchas the chip layer resolves (both documented at their fix
site):
- **Default linker script.** The pinned Arm GNU toolchain (newlib) injects no
  default linker script, so the app's board script applies cleanly. The app link
  passes its script with a **driver-level `-T`** (not `-Wl,-T`) regardless -- it
  is correct either way, and it is what the earlier Debian arm-none-eabi toolchain
  (picolibc) *required*, since picolibc's spec injected a default `-T picolibc.ld`
  unless it saw a driver-level `-T` (a `-Wl,-T` was invisible to that check and the
  two scripts collided at address 0). Keep the driver-level form. (See the `kickos`
  interface `target_link_options`.)
- **QEMU's DWT cycle counter is frozen.** `arch_clock_now` has NO armv7m fallback at
  all -- it is a required per-chip definition (`arch_armv7m.cc`, "Monotonic clock: NO
  armv7m default"), so a board that forgets it fails to LINK; `mps2` supplies it from
  the semihosting `SYS_CLOCK` (the monotonic clock source is legitimately
  chip-specific). The DWT `CYCCNT` read is a *different* seam, `arch_trace_now`, whose
  fallback TU (`arch/arm/armv7m/arch_trace_now_default.cc`) real silicon keeps; `mps2`
  displaces that too, deriving microseconds from `SYS_CLOCK`. Caveat: on QEMU the clock
  (host wall-time, 10 ms granularity) and the one-shot timer (SysTick counting
  virtual cycles) are **two uncorrelated timebases**, so sub-10 ms deadlines land
  up to 10 ms late and can cause a bounded re-arm churn until the coarse clock
  advances. This makes the QEMU gate a *functional* check, not a timing-accurate
  one; real silicon runs the clock and the compare off the same source.

## The RP2040 chip (`board`: `picopi`, chip `rp2040`, armv6m) -- hardware-validated

The Raspberry Pi Pico (RP2040, Cortex-M0+) is the first KickOS target **confirmed
running on real silicon** (`apps/blink` blinks the onboard LED on GP25). Three
things make it the most involved chip bring-up so far:

- **Flash second stage (boot2) + XIP.** The RP2040 executes in place from external
  QSPI flash, but only after a 256-byte second stage configures the Synopsys SSI.
  The bootrom copies those 256 bytes to SRAM, checks a **CRC-32/MPEG-2** over bytes
  0..251 (little-endian at 0xFC), and jumps in. `boot2.S` does the minimum
  (datasheet section 4.10.3): disable the SSI, program the "03h serial read per access"
  XIP mode (`CTRLR0=0x001f0300`, `SPI_CTRLR0=0x03000218`, BAUDR=4), re-enable, then
  set VTOR and hand off to the app vector table at 0x1000_0100. It is
  position-independent (runs from a bootrom-chosen SRAM address). The build wires
  the multi-stage image: assemble+link `boot2.S` (own `boot2.ld`, <=252 bytes) ->
  `objcopy -O binary` -> `cmake/rp2040_checksum.py` (appends the CRC, emits a
  `.boot2` data blob) -> into the chip archive, force-linked with `-Wl,-u`.
  - **Driver-level `-T`, again.** The boot2 *sub-link* passes `boot2.ld` with a
    driver-level `-T` (not `-Wl,-T`) for the same reason as the app link -- required
    under the old picolibc apt toolchain, retained (harmless) under newlib.
- **No PLL -- one 12 MHz crystal drives everything.** For a first bring-up the clock
  tree is deliberately minimal (no PLL sequencing): enable the XOSC, switch
  `clk_ref` to it (so `clk_sys` follows to 12 MHz -- precise SysTick,
  `SystemCoreClock=12e6`), and point `clk_peri` at it (precise UART baud). A
  1 MHz watchdog tick feeds the 64-bit system TIMER, which is `arch_clock_now`
  (the RAW halves, hi/lo/hi re-read -- no DWT on v6-M, and core-safe if core 1 is
  ever launched). Every poll is **bounded**: a dead crystal degrades to the ROSC
  default instead of hanging.
- **Reset-release ordering is load-bearing.** A peripheral's `RESET_DONE` only
  asserts once it has a running clock. IO_BANK0/PADS_BANK0/TIMER (clk_sys/clk_ref,
  live at reset) are released first; **UART0 is clocked by `clk_peri` and must be
  released *after* `clocks_init`** -- release it first and its `RESET_DONE` never
  asserts, hanging the boot with no sign of life. (This exact bug bit the first
  bring-up; the LED-bisection diagnostic localized it to the reset poll.)

No RP2040 model ships in mainline QEMU, so there is no CI gate; the image is
build-verified (boot2 CRC recomputed, `.boot2` at 0x1000_0000, vectors at
0x1000_0100) and confirmed by flashing a Pico (BOOTSEL + `picotool load -x`).
The board is always BOOTSEL-recoverable, so a wrong boot2/clock config cannot
permanently brick it.

## The RISC-V RV32IMAC arch (boards `qemu-riscv`->chip `virt`, `esp32c6-wroom`->chip `esp32c6`)

The first RISC-V ISA (ESP32-C6 + the QEMU `virt` run target), sharing one arch
(`arch/riscv/rv32imac/`) across two chips. Closest to the RX72M model: a single
save-frame, deferred switch.

- **Trap model** = ONE `mtvec` DIRECT-mode handler (`trap_entry`, switch.S) that
  saves the FULL interrupted context (28 GPRs + `mepc` + `mstatus`, 128 B) on the
  running thread's own stack, then demuxes on `mcause`: ecall (8/11), machine
  software / msip (the switcher), machine timer / mtip. `gp`/`tp` are not saved
  (link-time constant). ONE frame format for a voluntary block and a preemptive
  wake -- the RX/PendSV property.
- **Context switch** = deferred via the **CLINT machine software interrupt
  (`msip`)**. `arch_switch` records `g_arch_next` + pends msip; the physical swap
  ALWAYS happens in the msip trap (`.Lswitch`). Held off while an IrqLock masks
  `mstatus.MIE`. The CLINT base is chip-provided (`g_clint_msip`).
- **Syscall** = **`ecall`** -> `svc_trampoline` running **M-mode (privileged) on the
  caller's own stack** (mret with `MPP=M`), so a blocking dispatch's continuation
  is per-thread (the arch.h contract). The frame keeps the caller's `mstatus`
  (`MPP`=caller priv) for the return; `mepc`+4 skips the (4-byte) `ecall`.
- **Critical section** = `mstatus.MIE` (clear via `csrrci`, restore via `csrs`);
  `arch_in_isr` reads `g_isr_depth` (bumped only by the timer/external paths).
  `arch_idle_wait` = `wfi`.
- **Trace clock** = the `rdcycle` CSR (always present, 32-bit raw; `mcounteren`
  lets U-mode read it). **Clock/one-shot timer** are chip-provided (virt: CLINT
  `mtime`/`mtimecmp`; C6: SYSTIMER -- TODO(HW)).
- **PMP** -- a **permissive bootstrap entry** (pmpaddr0 NAPOT-all, pmpcfg0 = RWX,
  U-accessible) is set in `kickos_rv32_init`. RISC-V is fail-CLOSED: once PMP is
  implemented, a U-mode access with no matching entry FAULTS (unlike ARM, where
  unprivileged is unrestricted until the MPU clamps). So without this, an
  unprivileged thread can't fetch its first instruction. Per-task PMP enforcement
  landed at **M2**: `arch_mpu_apply` stashes the incoming set on switch-in and
  `kickos_arch_mpu_commit` programs the NAPOT PMP entries from the `.Lswitch` epilogue after the
  physical swap (the deferred-commit seam) (+ `arch_mpu_region_encodable` for the grant check) -- **enforced on qemu-riscv**; the
  ESP32-C6 image specifics (all-SRAM image, gp-relative small-data, code-from-RAM,
  and a separate APM/PMS bus permission unit) are still **blocked**, see
  `m2-readiness.md`. No F/D extension -> soft-float, so the switch banks no FP.
- **`gp` anchor for full-C++ under MPU** -- RISC-V small-data addresses globals as
  `gp + imm` from one `__global_pointer$`. For a full-C++ app under per-task
  enforcement the anchor MUST sit **inside the app's granted data region**: the
  runtime's small globals (`eh_globals`, `_impure_ptr`, the FDE registry heads) and a
  `-fexceptions` TU's `gp`-relative EH references (`DW.ref.*`, LSDA datarel) all live
  in that window, so an out-of-region anchor faults an unprivileged throw. Contract:
  link `PROVIDE(__global_pointer$ = ...)` within the app-data block, and compile the
  KickOS libs `-msmall-data-limit=0` so they emit no small-data and vacate the window
  (else granting it would hand a U-thread the kernel's own scheduler small-data). App
  TUs stay compiled *with* small-data so unwinding works. Folds into the app-data grant
  at +0 regions; `switch.S` is untouched (`gp` stays one link-time constant). ARM and RX
  have no small-data model and skip this.
- **`arch_irq_inject`** (fake-a-device-firing test/bench scaffolding) uses the
  **supervisor software interrupt** (`mip.SSIP`, `mcause`=1) as a private channel --
  the RISC-V analog of the host sim's `raise(SIGUSR1)`. The **PLIC has no
  software-generated interrupt** (unlike the Cortex-A GIC's SGIs; QEMU faithfully
  rejects a software pending-write), so a real device IRQ cannot be faked through
  it. Masking is a software bitmask (the sim's `irq_masked` twin); a raise on a
  masked line latches one-deep (redelivered at the next unmask). The bench's
  IRQ-entry-latency sample and the IRQ self-tests (`irq_thread_ctx` /
  `irq_as_event` / `irq_mask_coalesce`) run on `virt` this way.
  SSIP needs S-mode (present on the QEMU virt CPU); the C6 is M/U-only, so its
  inject routes to an interrupt-matrix "from-CPU" line at HW bring-up. A real
  device-interrupt *receive* path (a PLIC over `meip`) is a driver-era concern.

### The QEMU verification target (`board`: `qemu-riscv`, chip `virt`)

A runnable rv32imac target validates the arch on real emulated RISC-V:
`qemu-system-riscv32 -M virt -bios none -nographic -semihosting`. `-bios none`
runs our image directly in **machine mode** (no OpenSBI). The `virt` chip
(`arch/riscv/chip/virt/`) uses the standard CLINT (`mtime`/`mtimecmp` @ 10 MHz,
`msip`) + RISC-V **semihosting** for the console (`SYS_WRITEC`) and exit
(`SYS_EXIT_EXTENDED`) -- the mps2 model -- so it needs no UART; the image links to
run from DRAM at `0x8000_0000`. `ctest --preset qemu-riscv` boots `hello` and
asserts the ping-pong (reset -> scheduler(msip) -> ecall syscalls -> CLINT timer ->
semaphore reschedule). The C6 board (`esp32c6-wroom`, chip `esp32c6`, build-only) is flash-to-validate:
esptool image + real UART/SYSTIMER/watchdog/CLINT register values are the HW pass.

---

## Verification status

The arch backend **cross-compiles clean** for Cortex-M4/M3, and the **spike is
empirically validated on QEMU Cortex-M4**: `ctest --preset qemu` boots `hello`
and asserts the two userspace threads ping-pong -- exercising reset -> C-runtime ->
scheduler start (PendSV first switch) -> **SVC-trampoline syscalls from both a
privileged and an unprivileged thread** -> SysTick one-shot driving `sleep` ->
semaphore block/wake reschedule, all on a real Cortex-M4 core. The #1 M1 risk is
retired.

Since then the chip layer has been brought up on real hardware across the fleet -- `mk64f` (M1
baseline + SYSMPU enforcement + the first unprivileged MMIO drivers), **`rp2040` running on a
real Raspberry Pi Pico** (see the RP2040 section), and the rest of the table above. The
STM32 chips reuse the `mps2`/`mk64f`/`rp2040` patterns.

Hardware MPU enforcement is **done** (M2): the cross-domain trap is silicon-proven on SYSMPU,
PMSAv6-M/v7/v8, RISC-V PMP and the RX MPU. For a new port that means enforcement is part of the
seam you implement, not a later milestone -- `arch_mpu_apply` (stash at the switch decision),
`kickos_arch_mpu_commit` (program from the switch epilogue, after the physical swap),
`arch_mpu_region_encodable`, and `arch_reserved_blocks`, which has no fallback TU so omitting it
is a link error. See `architecture.md` (Memory domains) and `invariants.md`
(`mpu-apply-on-every-switch-in`, `grant-refuses-kernel-reserved-blocks`).
