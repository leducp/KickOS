<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# RP2350 Hazard3 (RISC-V) port -- feasibility / design spike

**EXPLORATORY -- NOT A CONTRACT. NO IMPLEMENTATION.** A feasibility study for running
KickOS on the RP2350's **RISC-V Hazard3** cores, as a sibling of the existing ARM
Cortex-M33 port (`arch/arm/chip/rp2350/`, `docs/design-rp2350.md`). Register facts that
would be clean-room from the datasheet are marked **[to-verify: RP-008373 / Hazard3
docs]** where I lack a primary source in-repo -- the datasheet is not in the tree, and
this doc invents no addresses. It cites live code (`file:line`) for everything already
present.

The hook: the RP2350 ships BOTH a dual Cortex-M33 (ARMv8-M) pair AND a dual Hazard3
(RV32IMAC + custom extensions) pair on one die; which ISA runs is selected at boot (OTP
`ARCHSEL`). KickOS already has a silicon-proven `rv32imac` arch layer (ESP32-C6, PMP
enforcement on hardware; qemu-virt). So the SAME physical RP2350 board could boot KickOS
as either an ARM MPU kernel (`pizero2350`, done to build+image-inspection in
`design-rp2350.md`) or a RISC-V PMP kernel (Hazard3) -- a uniquely clean demonstration
that the microkernel core is arch-neutral. This spike scopes what that second boot costs.

## 0. Outline

1. Verdict + the crux risk
2. What already exists (the two reuse surfaces: `rv32imac` arch-generic vs chip-specific,
   and the core-agnostic RP2350 SoC facts)
3. Boot model -- ARCHSEL selection, Hazard3 reset, IMAGE_DEF RISC-V flag, linker/XIP
4. Arch reuse -- generic `rv32imac` vs Hazard3 deltas; the interrupt + timer + IPI story
   (the crux)
5. Peripheral reuse -- the core-agnostic SoC blocks that reuse the ARM backend verbatim
6. PMP enforcement on Hazard3 -- the M/U split KickOS relies on
7. Concrete port plan -- preset, chip backend, bring-up milestone, flashing path
8. Risks / unknowns / roadmap fit

---

## 1. Verdict

**Feasible, and structurally cheaper than the first RISC-V port was -- but NOT a weekend
spike. Milestone-class-small: a strong week of in-tree work plus a silicon bring-up tail,
gated on one crux.** The reasons it is cheap:

- The `rv32imac` arch layer is already **ISA-generic and chip-parameterised** by design
  (`arch/riscv/rv32imac/arch_rv32imac.cc:5-11` names exactly the seam a chip fills). A
  Hazard3 port is a NEW **chip backend** under `arch/riscv/chip/rp2350/`, not a new arch.
- The core-agnostic half of the SoC (clock tree, UART0 PL011, 64-bit TIMER0, resets,
  pads, boot block) is **already written and reasoned-about** in the ARM backend
  (`arch/arm/chip/rp2350/chip_rp2350.cc`) -- same registers, same sequences, same
  datasheet sections. The Hazard3 chip backend reuses that register knowledge verbatim.
- The ESP32-C6 already taught the two hardest RISC-V-chip lessons the C6 and Hazard3
  share: a **M/U-only core with no S-mode** (so the SSIP software-inject test channel
  needs a chip override, `arch_rv32imac.cc:325-346`, `chip_esp32c6.cc:432-444`) and a
  **non-standard external-interrupt controller** reached through chip-override hooks
  (`arch_rv_hw_unmask`/`arch_rv_ext_eoi`/`kickos_rv_ext_dispatch_dev`).

**The crux risk (Section 4): the Hazard3 interrupt + timer + software-IPI fabric.** The
`rv32imac` scheduler rests on THREE core-local facilities the chip must provide:
(a) a machine **software interrupt** (`g_clint_msip`) to pend the deferred context switch;
(b) a 64-bit **machine timer** (`mtime`/`mtimecmp`) for the tickless clock;
(c) an **external interrupt** path for device IRQs. On the ESP32-C6 all three came from a
memory-mapped CLINT + PLIC (`chip_esp32c6.cc:74-78, 196-202`). **Hazard3 does NOT use a
standard PLIC** -- it integrates its own external-interrupt controller exposed through
custom Hazard3 CSRs, and the RP2350 supplies a platform timer + software-interrupt path
of its own shape (SIO / a RISC-V platform-timer block). Whether (a)/(b)/(c) map onto
Hazard3 as cleanly as the C6's CLINT+PLIC is the single largest unknown and the thing to
pin against the datasheet first. It is de-riskable: the arch layer already isolates all
three behind a pointer (`g_clint_msip`) and weak chip hooks, so the port is a
chip-backend fill, not an arch-layer surgery -- **unless** Hazard3's software-interrupt or
timer semantics differ enough to force a small arch seam change (e.g. a `g_msip`-style
indirection that is a function rather than a single MMIO word).

Recommended framing: **an M4/M5-era demonstrator** (Section 8). It de-risks the
"arch-neutral core" claim concretely (one board, two ISAs, one microkernel) and pairs
naturally with the M5 SMP work, since Hazard3 has its own dual-core story on the same die.

---

## 2. What already exists (the reuse surfaces)

### 2.1 The `rv32imac` arch layer -- what is generic vs chip-specific

`arch/riscv/rv32imac/arch_rv32imac.cc` is the ISA-generic half; `switch.S` holds the
context-switch/trap/trampoline asm. The header comment (`:5-11`) states the seam
explicitly. Concretely:

**Arch-generic (reused by Hazard3 UNCHANGED):**
- Context model + first-frame fabrication (`arch_context_init`, `:123-152`): a 32-word /
  128-byte frame, `mret` into `entry(arg)` at the chosen privilege, `mstatus` MPIE|MPP.
  gp/tp are link-time constants, not saved (`:53-58`).
- The deferred-switch model (`arch_switch`, `:159-164`): record `g_arch_next`, write
  `*g_clint_msip = 1` to pend the machine software interrupt; the physical swap always
  happens in the msip trap. This is the RX-SWINT / ARM-PendSV lineage.
- Critical section = `mstatus.MIE` (`arch_irq_save`/`restore`, `:167-180`); `g_isr_depth`
  the `IPSR!=0` analog (`:182-185`); `wfi` idle (`:441-444`).
- The mtvec **vectored** trap install + `mie` enable of msip(3)/mtip(7)/ssip(1) +
  bootstrap PMP (`kickos_rv32_init`, `:534-576`).
- The PMP NAPOT backend (`arch_mpu_apply` stashes the set, `kickos_arch_mpu_commit` writes
  pmpaddr/pmpcfg from the switch epilogue; 8 entries; `arch_mpu_min_region` = 8;
  `arch_mpu_region_encodable`).
- The fault reporter + U-mode-access-fault -> `kickos_isr_fault` routing (`:451-522`).
- The software-inject IRQ scaffolding + the chip-override hook set (`:308-438`).

**Chip-specific (the Hazard3 backend must supply):**
- `g_clint_msip` = a pointer to the machine-software-interrupt-pending word (`:88-90`);
  set in the chip's `arch_init`.
- `arch_clock_now` / `arch_timer_arm` / `arch_timer_disarm` (the `mtime`/`mtimecmp`
  tickless clock) -- see the two existing bodies (`chip_esp32c6.cc:392-422`,
  `chip_virt.cc:120-153`).
- `arch_console_write` (+ `_sync`), `arch_init`, `arch_shutdown`, `Reset_Handler`,
  `startup.S`, the linker script.
- Optional overrides where the core diverges from the SiFive/QEMU baseline:
  `arch_rv_has_mcounteren` (C6 returns 0, `:427`), `arch_rv_inject_deliver`,
  `arch_rv_ext_eoi`, `kickos_rv_ext_dispatch_dev`, `arch_rv_hw_unmask/mask`.

The C6 backend is the template: it supplies its own memory-mapped CLINT
(`chip_esp32c6.cc:74-97`), routes device IRQs through the PLIC + interrupt matrix (not a
standard PLIC-only path), and overrides the M/U-only and no-`mcounteren` cases. **The
Hazard3 backend is the same shape with RP2350 hardware facts substituted.**

### 2.2 The core-agnostic RP2350 SoC facts (already in the ARM backend)

`arch/arm/chip/rp2350/chip_rp2350.cc` already encodes, clean-room from RP-008373, the SoC
peripherals that are **identical regardless of which core executes** (the RP2350 is
homogeneous dual-M33 OR dual-Hazard3; the peripherals are the same silicon either way):

- **Clock tree** (`clocks_init`, `:250-304`): XOSC 12 MHz (`0x40048000`), PLL_SYS to
  150 MHz (`0x40050000`, 12/1*125=1500 VCO /5/2), the **TICKS** TIMER0 generator
  (`0x40108000`, the RP2350-new common tick block, not the RP2040 watchdog tick),
  clk_peri tracking clk_sys, dead-crystal ROSC fallback.
- **UART0 PL011** at `0x40070000` (`:164-322`) -- baud divisors recomputed for 150 MHz
  (IBRD/FBRD 81/24), the buffered-console-TX backend, the **PADS ISO gotcha** (pads reset
  isolated, `:160`, `:309-314`).
- **64-bit TIMER0** microsecond monotonic at `0x400b0000` via non-latching RAW halves
  (`arch_clock_now`, `:405-420`; `arch_trace_now`, `:425-428`).
- **RESETS** reset-release ordering (`:358-370`) -- UART0 released only after its clk_peri
  is live (the RP2040 lesson).
- **IMAGE_DEF boot block + vector pin** (Section 3).

`design-rp2350.md:196-205` already asserts **~70% of the two chip backends is shared** and
recommends factoring the clock/UART/GPIO/TIMER register sequences into a common unit when
the second (Hazard3) consumer lands, rather than copy-pasting. This spike agrees: see
Section 7.2.

---

## 3. Boot model

### 3.1 ARCHSEL: how the RP2350 chooses RISC-V vs ARM

From `design-multicore.md:119-130` (RP-008373-DS section 3.9, already digested): each of
the two processor "sockets" is filled at reset by EITHER a Cortex-M33 OR a Hazard3, per
the OTP **`ARCHSEL`** register; the unused processor is held in reset with clocks gated.
Default = Arm. Setting **`CRIT1_BOOT_ARCH`** (an OTP flag) selects RISC-V. `ARCHSEL` is
sampled ONLY at reset, so a watchdog reset can flip architecture in software -- **relevant
to KickOS**: a "boot the other ISA" demo can be driven by writing ARCHSEL then triggering
a watchdog reset, no re-flash. Mixed Arm+RISC-V (one socket each) is physically possible
but requires two separate images and is explicitly exotic (DS 3.9.2) -- **out of scope**;
KickOS targets homogeneous dual-Hazard3.

Practical selection paths **[to-verify: RP-008373 3.9.x + picotool docs]**:
- picotool can set the boot architecture / relevant OTP or supply an ARCHSEL override on
  boards that expose it; and the bootrom honors the IMAGE_DEF CPU flag (below).
- Development boards commonly expose ARCHSEL via the bootrom + picotool rather than
  burning OTP fuses irreversibly. Confirm the exact non-permanent selection mechanism for
  the Waveshare Pi-Zero board before committing to a demo flow.

### 3.2 Hazard3 reset + control transfer

`design-rp2350.md:186-193` already sketched this. The RP2350 **removed boot2** (no
CRC-checked 256-byte second stage, unlike RP2040): the bootrom does best-effort QSPI XIP
setup, then scans the first 4 KiB of the flash image for a block loop containing a valid
**IMAGE_DEF** (DS 5.9.5). The boot mechanism is **shared** with the ARM path; the only
delta is the IMAGE_DEF `image_type_flags` **CPU field = RISC-V(1)** instead of ARM(0)
(`0x1121` vs the ARM `0x1021` in `startup.S`; `design-rp2350.md:191-192`).

The RISC-V entry convention differs from ARM's "SP@[base+0], reset PC@[base+4] vector
table" **[to-verify: RP-008373 5.9.3.4]**: a RISC-V image is entered at a plain entry
point, not via an Arm-style vector table at the image base. So the IMAGE_DEF likely needs
an explicit **`ENTRY_POINT` item** (or the RISC-V reset convention) rather than relying on
the Arm vector-table-at-base default. This must be pinned against the datasheet; it is a
different `startup.S` prologue (a `_start` label that sets gp/sp, exactly like
`arch/riscv/chip/esp32c6/startup.S:13-25`, NOT an ARM `.isr_vector` table).

### 3.3 XIP-from-flash vs SRAM; the linker

The ARM `rp2350.ld` runs the image **XIP in place from QSPI flash** at `0x1000_0000` with
SRAM at `0x2000_0000` (`design-rp2350.md:31-47`). The C6/virt RISC-V ports instead run
**all-from-SRAM** (`esp32c6.ld:30-35`: image links to run from HP SRAM, ROM loader copies
segments in, LMA==VMA). For Hazard3 the choice is open and worth an explicit decision:

- **XIP-from-flash (recommended, matches the ARM sibling):** the fairest apples-to-apples
  demo -- same board, same flash layout, same IMAGE_DEF mechanism, only the CPU flag and
  the reset prologue differ. Requires the RISC-V linker script to place `.text`/`.rodata`
  in the flash XIP region and `.data`/`.bss`/stacks in SRAM (LMA in flash, VMA in SRAM,
  `Reset_Handler` copies `.data`) -- unlike the C6's LMA==VMA all-SRAM script, and unlike
  the RISC-V `.appdata` NOLOAD-window machinery, which must be re-homed for a flash LMA.
- **All-SRAM:** simpler PMP alignment story (the C6/virt scripts already carve pow2 NAPOT
  code + appdata regions from an 8 MiB-aligned SRAM origin, `esp32c6.ld:40-58`), but does
  not exercise XIP and diverges from the ARM sibling. A reasonable **first bring-up**
  target (get to a console fast), then move to XIP for the real demo.

**PMP-vs-XIP interaction to flag:** the RISC-V PMP NAPOT code region must be a naturally
aligned power of two (`arch_mpu_region_encodable`, `arch_rv32imac.cc:298-305`). The C6
script leans on the SRAM origin being 8 MiB-aligned so any pow2 sub-block is aligned
(`esp32c6.ld:22-27`). For an XIP build the code region sits in flash at `0x1000_0000`
(16 MiB-aligned -- fine), but the linker script must be re-derived; this is the same class
of work `design-riscv-gp-split.md` covers for the `.appdata`/gp window, which also needs
re-homing when the data LMA moves to flash.

---

## 4. Arch reuse: generic vs Hazard3 -- the interrupt / timer / IPI crux

This is the section that decides the effort. The `rv32imac` scheduler needs three
core-local facilities. For each: what the arch layer assumes, what the C6 provided, and
the Hazard3 unknown.

### 4.1 The software interrupt (deferred context switch) -- `g_clint_msip`

- **Arch assumption:** `arch_switch` writes `*g_clint_msip = 1` (a single MMIO word) to
  pend the machine software interrupt; the msip trap (`switch.S`) does the swap and clears
  it (`arch_rv32imac.cc:88-90, 159-164`). `mie.MSIE` (bit 3) is enabled in
  `kickos_rv32_init`.
- **C6:** a memory-mapped CLINT MSIP word at `0x2000_1800` (`chip_esp32c6.cc:75`), set as
  `g_clint_msip` in `arch_init`.
- **Hazard3 unknown [to-verify: RP-008373 + Hazard3 docs]:** Hazard3 supports the standard
  RISC-V machine software interrupt (`mip.MSIP`/`mie.MSIE`), but the RP2350 mechanism to
  *set* it per-core is the question. Two plausible shapes:
  (i) a CLINT-like MSIP register (SIO or a platform block) -- then `g_clint_msip` is just
  a different address, zero arch change; or
  (ii) Hazard3 exposes software-interrupt assertion via a custom CSR / a SIO doorbell
  rather than a writable MMIO word -- then the arch's "write a word through a pointer"
  contract must widen to "call a chip hook" (a small, mechanical arch seam change:
  `arch_switch` calls `arch_rv_pend_switch()` weak-overridable, default = the MMIO-word
  write for C6/virt). **This is the most likely place the arch layer needs a touch.**

### 4.2 The machine timer (tickless clock) -- `mtime`/`mtimecmp`

- **Arch assumption:** a 64-bit `mtime` read (`arch_clock_now`) and an absolute
  `mtimecmp` write (`arch_timer_arm`), with `mie.MTIE` (bit 7) and `mtip` driving the
  tickless tick. RV32 64-bit compare uses the park-high-half write discipline
  (`chip_virt.cc:141-146`).
- **C6:** its CLINT block's MTIME/MTIMECMP at `0x2000_1808`/`0x2000_1810`, core-clocked at
  the PLL frequency (~160 MHz), with the 25/4 ns-per-tick ratio (`chip_esp32c6.cc:76-97`).
- **Hazard3 unknown [to-verify: RP-008373 3.1.8]:** `design-multicore.md:210-211` already
  notes "RP2350 adds a RISC-V platform timer (DS 3.1.8)". So a RISC-V-facing
  `mtime`/`mtimecmp` DOES exist -- the question is its base address, its tick rate/source
  (does it derive from the same clk_ref 1 MHz tick the ARM TIMER0 uses, or a separate
  RISC-V platform-timer clock?), and whether it is per-core. If it is a clean
  `mtime`/`mtimecmp` pair, the timer body is a near-copy of the C6/virt code with a
  different base and ns-per-tick constant. **Lower risk than 4.1.** Note the RP2350 also
  has the ARM-side 64-bit TIMER0 (`0x400b0000`) which is a system peripheral -- KickOS
  could read TIMER0 for `arch_clock_now` (core-agnostic MMIO, reuse the ARM body verbatim)
  and use the RISC-V platform `mtimecmp` only for the next-event interrupt. That split is
  attractive: the monotonic clock reuses proven core-agnostic code, and only the compare
  interrupt needs the RISC-V timer.

### 4.3 The external-interrupt path (device IRQs) -- THE crux

- **Arch assumption:** device IRQs arrive as a machine external interrupt demuxed in
  `switch.S`; the arch provides a software-inject test channel (SSIP) plus weak chip hooks
  (`arch_rv_hw_unmask/mask`, `arch_rv_ext_eoi`, `kickos_rv_ext_dispatch_dev`) for a real
  controller (`arch_rv32imac.cc:308-438`).
- **C6:** it does NOT use a standard PLIC-only model -- it routes device sources through
  an **interrupt matrix (INTMTX)** to per-CPU interrupt IDs, configured via a
  PLIC-at-`0x2000_1000` window, with the M/U-only core having no SSIP so the inject
  channel is overridden to a real FROM_CPU source (`chip_esp32c6.cc:179-250, 432-518`).
- **Hazard3 [to-verify: Hazard3 docs + RP-008373]:** Hazard3 has its **own external
  interrupt controller integrated into the core**, exposed through **custom Hazard3 CSRs**
  (the Hazard3 IRQ array: per-IRQ enable/pending/priority CSRs, e.g. the `meiea`/`meipa`/
  `meifa`/`meie`-family and a `meinext`-style claim -- **exact CSR names/encodings
  to-verify**). It is **NOT a memory-mapped standard PLIC.** This is the crux:
  - **Good news:** the arch layer already assumes a non-standard controller reached
    through the weak hooks (that is exactly why the C6 fit without arch surgery). A
    Hazard3 backend implements `arch_rv_hw_unmask/mask` to program the Hazard3 IRQ CSRs,
    `kickos_rv_ext_dispatch_dev` to claim/EOI, and `arch_rv_inject_deliver` for the test
    channel. The demux in `switch.S` keys on `mcause`; Hazard3's external interrupt is the
    standard `mcause` machine-external cause **[to-verify it does not use a C6-style
    custom mcause=ID scheme]**.
  - **Risk:** if Hazard3's external IRQ claim/EOI is done via a CSR read (like the C6's
    custom scheme needing a `switch.S` demux branch, `arch_rv32imac.cc:420-433`), the
    trap-entry asm may need a small new branch, i.e. a `switch.S` edit, not just C hooks.
    The M/U-only + custom-controller combination is precisely the C6 pattern, so the
    *shape* of the work is known; only the *register/CSR facts* are new.
  - **Software IPI for SMP later:** the RP2350 SIO doorbells / FIFOs
    (`design-multicore.md:132-145`) are the cross-core notify primitive and are
    core-agnostic; they are an M5-SMP concern, not needed for the single-core bring-up.

### 4.4 Privilege modes + extensions

- **M/U split:** KickOS's PMP enforcement relies on M-mode kernel / U-mode threads
  (`arch_rv32imac.cc:462`, `kickos_rv32_init` bootstrap PMP). Hazard3 on RP2350 implements
  M and U modes **[to-verify Hazard3 has_user is enabled on RP2350]**. Like the C6 it is
  expected to be **M/U-only (no S-mode)** -- so the SSIP inject channel is a no-op and
  needs the chip override, exactly the C6 lesson (`chip_esp32c6.cc:432-436`).
- **Extensions:** Hazard3 is RV32IMAC plus **B (bit-manip: Zba/Zbb/Zbc/Zbs)** and custom
  Zc*/Xh3* extensions. RV32IMAC is the safe baseline; the arch layer is agnostic to the
  supersets. A `-march` knob (board descriptor `KICKOS_MCPU`) could opt into `rv32imac_zb*`
  for code density, but the pinned RISCStar multilib is soft-float rv32imac/ilp32
  (`toolchain-riscv-none-elf.cmake:5-20`) -- **staying on plain rv32imac reuses the exact
  multilib the C6/virt use, zero toolchain risk.** No F/D -> soft-float, switch banks no FP
  (same as C6). Recommend: bring up on rv32imac, treat B as a later optimisation knob.

---

## 5. Peripheral reuse -- core-agnostic SoC blocks

These reuse the ARM `chip_rp2350.cc` register knowledge **verbatim** (same silicon, same
addresses, executed by whichever core is live):

| Peripheral | ARM backend source | Reuse for Hazard3 |
|---|---|---|
| Clock tree (XOSC/PLL_SYS/CLOCKS/TICKS) | `chip_rp2350.cc:250-304` | verbatim; same PLL to 150 MHz, same ROSC fallback |
| UART0 PL011 console `0x40070000` | `:164-322` | verbatim; incl. the PADS ISO-bit gotcha (`:160,309-314`) and the buffered console-TX backend |
| 64-bit system TIMER0 `0x400b0000` | `arch_clock_now :405-420` | verbatim -- candidate `arch_clock_now` source (see 4.2) |
| RESETS release ordering `0x40020000` | `:358-370` | verbatim |
| PADS / IO_BANK0 pin mux | `:150-163, 306-322` | verbatim |
| IMAGE_DEF boot block | `startup.S` / `design-rp2350.md:78-99` | CPU flag delta only (Section 3.2) |

**Core-facing pieces that differ** (the Hazard3 chip backend rewrites these, small):
Reset prologue (RISC-V `_start` sets gp/sp/tp, no VTOR/CPACR/FPU enable), the trap/CSR
init (`kickos_rv32_init` instead of `kickos_armv7m_init`), the interrupt controller
programming (Hazard3 IRQ CSRs instead of NVIC), the timer/software-interrupt bases
(Section 4), and the MPU (PMP instead of PMSAv8).

This is why `design-rp2350.md:196-205` recommends factoring the shared register sequences.
**Recommendation for the actual port: extract the clock/UART/TIMER0/RESETS/PADS register
sequences into a small `static inline` header** (e.g.
`arch/*/chip/rp2350_common/rp2350_soc.h`) that BOTH the ARM and RISC-V chip backends
include -- not a shared `.a` (the two backends compile under different toolchains/ISAs).
A header of `constexpr` addresses + `static inline` sequences is toolchain-neutral and
kills the copy-paste. This is a **cheap refactor of the existing ARM backend**, best done
as step 1 of the port (Section 7).

---

## 6. PMP enforcement on Hazard3

KickOS's RISC-V isolation is the 8-entry NAPOT PMP backend
(`arch_rv32imac.cc:245-305`), already **silicon-proven on the C6** (bounded PMP NAPOT
enforcement, per the C6 backend header `:20-21`), with the full-C++-under-PMP gp-window
story worked out in `design-riscv-gp-split.md`.

- **Does Hazard3 implement PMP? Yes [to-verify count: RP-008373 / Hazard3 config].**
  Hazard3 implements RISC-V PMP; the region count is a synthesis parameter. The RP2350
  Hazard3 configuration's PMP region count must be confirmed -- the arch backend uses **8**
  entries (`kickos_arch_mpu_commit` writes pmpaddr0..7 from the stash). If Hazard3-on-RP2350
  exposes fewer than 8 PMP regions, the commit must clamp (it already only writes what it
  builds, but the domain region budget assumes 8; verify against
  `arch_domain_static_regions`).
- **The M/U bypass model holds:** M-mode kernel bypasses unlocked PMP entries (the
  privileged-background analog, `:214-216, 264`); U-mode threads are clamped per-thread on
  switch-in. This is core-independent RISC-V-privileged-spec behavior.
- **The C6 NAPOT gotchas to re-verify on Hazard3:** the C6 does NOT honor the all-ones
  NAPOT match-everything idiom, so the bootstrap entry uses **TOR** with
  pmpaddr0=0xFFFFFFFF (`:563-575`). Whether Hazard3 honors all-ones NAPOT is unknown; the
  TOR bootstrap is the safe, already-written path either way -- reuse it.
- **Fail-closed:** RISC-V PMP is fail-closed (a U-mode access with no matching entry
  faults, `:564-568`) -- the bootstrap permissive entry is mandatory before the first
  U-mode fetch. This is arch-generic, reused as-is.
- **gp-window / full-C++ under PMP:** `design-riscv-gp-split.md` Option 4 (empty the kernel
  gp side, anchor `__global_pointer$` inside the app grant, force-reload gp on dispatch) is
  backend-shared -- it applies to the Hazard3 linker script identically. The XIP-vs-SRAM
  LMA choice (Section 3.3) is the only new wrinkle for the `.appdata` window.

**Verdict on PMP:** the lowest-risk part of the port. The backend is written and proven;
Hazard3 needs a region-count confirmation and the same NAPOT/TOR re-verification the C6
already went through.

---

## 7. Concrete port plan

### 7.1 New preset + board descriptor

- New board `boards/pizero2350-riscv/board.cmake`:
  `KICKOS_ARCH_FAMILY riscv`, `KICKOS_ARCH rv32imac`, `KICKOS_CHIP rp2350`,
  `KICKOS_MCPU -march=rv32imac_zicsr -mabi=ilp32` (identical baseline to
  `boards/qemu-riscv/board.cmake:22`). The board resolver + RISC-V toolchain already route
  family->chip glob->include path with zero code changes
  (`arch/CMakeLists.txt:140-144`, `toolchain-riscv-none-elf.cmake:31-46`).
- New preset in `cmake/presets/riscv.json` (e.g. `pizero2350-riscv` +
  `pizero2350-riscv-st`), mirroring the `esp32c6-wroom` entries (`riscv.json:28-41`).
- **Naming note:** the ARM board is `pizero2350` (`boards/pizero2350/`), chip `rp2350`.
  The RISC-V board is a NEW board descriptor on the SAME chip. But `KICKOS_CHIP=rp2350`
  currently resolves to `arch/arm/chip/rp2350` because the family is `arm`. For RISC-V,
  family=`riscv` routes to `arch/riscv/chip/rp2350` (`arch/CMakeLists.txt:140`). So the two
  chip backends coexist as `arch/arm/chip/rp2350/` and `arch/riscv/chip/rp2350/` -- the
  family segment disambiguates. No collision. Good.

### 7.2 New chip backend `arch/riscv/chip/rp2350/`

Files (mirroring `arch/riscv/chip/esp32c6/`):
- `startup.S` -- RISC-V `_start` (gp/sp/tp), call `Reset_Handler`. Model on
  `esp32c6/startup.S`. Plus the IMAGE_DEF block with the RISC-V CPU flag (Section 3.2)
  and, if XIP, the flash placement.
- `chip_rp2350.cc` -- `arch_init` (set `g_clint_msip`/timer base per Section 4, program the
  Hazard3 IRQ controller, `kickos_rv32_init`), `Reset_Handler` (`.data`/`.bss` +
  `.appdata` copy, init_array, `arch_init`, `kmain`), the clock/UART/TIMER0 bodies
  **via the shared SoC header (Section 5)**, `arch_console_write`/`_sync`, `arch_shutdown`,
  and the C6-style overrides (`arch_rv_has_mcounteren`, inject/EOI/dispatch hooks).
- `rp2350.ld` -- RISC-V linker script (XIP flash + SRAM, or all-SRAM first), the two
  NAPOT regions for PMP, the gp window per `design-riscv-gp-split.md`.
- `mpu.cmake` -- the KICKOS_HAVE_MPU opt-in (model on `esp32c6/mpu.cmake`).

**Step 0 refactor (do first):** extract the core-agnostic clock/UART/TIMER0/RESETS/PADS
register sequences from `arch/arm/chip/rp2350/chip_rp2350.cc` into a shared
`static inline` header both backends include (Section 5). This is an in-tree,
build-only-verifiable change to the existing ARM board -- do it before the RISC-V backend
so the second consumer reuses rather than copies.

### 7.3 Minimum bring-up milestone (staged, each a checkpoint)

1. **Build + link + image-inspection** (no bench, like the ARM `pizero2350` today): the
   RISC-V ELF links, the IMAGE_DEF block decodes with the RISC-V CPU flag, the vector/entry
   is where the bootrom expects. `KICKOS_HAVE_MPU=0` first.
2. **Boot -> console** on silicon: ARCHSEL to RISC-V, flash, see the KickOS banner on UART0
   GP0/GP1 (the same wire the ARM port uses). This proves the boot model + clock tree +
   UART reuse. The C6 `KICKOS_C6_EARLY_MARK` technique (`chip_esp32c6.cc:127-145`) is worth
   porting for early-boot-hang localisation.
3. **Scheduler alive:** timer tickless clock + deferred switch working -> `selftest`
   passes with **mpu off** (privilege + syscall only, the M1 posture). This is where the
   Section 4 crux (software-interrupt + timer + external-IRQ fabric) is actually validated.
4. **PMP enforcement:** `KICKOS_HAVE_MPU=1`, the `mpu_fault` selftest (an ungranted U-mode
   access faults and is reported), matching the C6.
5. **Full-C++ under PMP** (optional, later): the `design-riscv-gp-split.md` gp-in-appdata
   layout, cxxtest.

Stage 1 is doable now, entirely in-tree. Stages 2-4 are silicon-gated (bench + ARCHSEL
access). Stage 3 is the make-or-break.

### 7.4 Flashing path

- **picotool understands RISC-V RP2350 images [to-verify picotool version supports
  RISC-V]:** the RP2350 UF2 family ID and IMAGE_DEF CPU flag distinguish a RISC-V image;
  picotool + the BOOTSEL USB mass-storage path are the same as the ARM flow. The board is
  **always BOOTSEL-recoverable** (`chip_rp2350.cc:28-31`), so a wrong clock/boot/ARCHSEL
  config cannot brick it.
- **UF2 emission:** the ARM port emits `.bin`/`.hex` and defers `.uf2`
  (`design-rp2350.md:214-215`); the RISC-V port needs the RP2350 family-id + the uf2 packer
  with the RISC-V flag. Same deferred item, add when bench access exists.
- **ARCHSEL flip flow:** confirm whether the demo re-flashes per ISA or uses the
  watchdog-reset ARCHSEL flip (Section 3.1) to switch a resident board between the ARM and
  RISC-V images without re-flashing. The latter is the compelling demo.

---

## 8. Risks / unknowns + roadmap fit

### 8.1 Ranked unknowns (all to-verify against RP-008373 + Hazard3 docs -- not in-repo)

1. **[CRUX] Hazard3 external-interrupt controller (4.3):** custom Hazard3 IRQ CSRs, NOT a
   standard PLIC. Risk it forces a small `switch.S` demux edit (claim/EOI via CSR) rather
   than pure C hooks. The C6 already needed a `.Lext`/`.Lextdev` demux, so the arch has
   precedent -- but the CSR facts are new. Pin this FIRST; it gates the effort estimate.
2. **Software-interrupt assertion path (4.1):** is the deferred-switch pend a writable
   MMIO word (`g_clint_msip` reused as-is) or a CSR/SIO doorbell (needs a small arch hook
   `arch_rv_pend_switch`)? Most likely place the arch layer itself needs a touch.
3. **RISC-V platform timer (4.2):** base, rate, per-core-ness. Lower risk -- a
   `mtime`/`mtimecmp` pair is expected (DS 3.1.8); the ARM TIMER0 is a fallback
   `arch_clock_now` source.
4. **RISC-V boot entry convention (3.2):** IMAGE_DEF `ENTRY_POINT` vs a reset default;
   different `startup.S` prologue. Build-time verifiable once the datasheet section is read.
5. **PMP region count (6):** confirm 8 on RP2350-Hazard3.
6. **ARCHSEL non-permanent selection + picotool RISC-V support (3.1, 7.4):** the demo-flow
   ergonomics.
7. **M/U-only (no S-mode) (4.4):** expected (C6 precedent); confirm `has_user`.

### 8.2 What is NOT a risk (de-risked by prior work)

- The scheduler/switch/trampoline/PMP arch code -- generic, silicon-proven on the C6.
- The SoC peripherals (clock/UART/TIMER0/pads/resets) -- written + reasoned in the ARM
  backend, reused verbatim.
- The M/U-only + custom-controller + no-mcounteren chip pattern -- the C6 walked it.
- Full-C++-under-PMP -- the gp-split design is backend-shared and done.

### 8.3 Effort verdict

**Not a weekend spike; a focused ~1-week in-tree effort + a silicon bring-up tail, gated
on reading the RP2350/Hazard3 interrupt-controller chapter.** In-tree (buildable today):
the shared-SoC-header refactor, the board descriptor + preset, the chip backend skeleton,
the linker script, Stage-1 image inspection. Silicon-gated: Stages 2-4, dominated by
unknown #1 (the interrupt fabric). If the Hazard3 IRQ controller maps cleanly onto the
existing weak-hook seam (as the C6's INTMTX+PLIC did), the tail is short; if it needs a
`switch.S` demux edit or an `arch_switch` pend-hook, add a few days for the arch touch plus
re-validating every existing RISC-V board (C6, virt) against the widened seam.

### 8.4 Roadmap fit

- **M4/M5-era demonstrator.** The DECIDED roadmap order (`design-driver-era-scope.md:424+`)
  is driver-era = M4, SMP = M5, MMU = M6. The Hazard3 port is not gated by the driver era
  and does not gate it; it is a **cross-ISA validation demonstrator** that fits best
  alongside M5 SMP:
  - It **de-risks the "arch-neutral core" claim** concretely and publicly: one physical
    board, two ISAs (ARM MPU kernel and RISC-V PMP kernel), one microkernel source.
    `design-multicore.md:147-151` already calls RP2350 "a rare cross-ISA test vehicle for
    the same SMP/AMP code paths."
  - It **pairs with M5 SMP:** Hazard3 has its own dual-core story on the same die (SIO
    doorbells/FIFOs, `design-multicore.md:132-145`), so the SMP per-CPU-seam + BKL work
    (`design-multicore.md:216-276`) can be validated on dual-Hazard3 AND dual-M33 from one
    board -- the Phase-3 "cross-ISA CI matrix" that doc envisions.
- **Cheapest standalone slice:** even single-core (no SMP), the Stage 1-3 bring-up alone
  delivers the cross-ISA demo. It can land as an orthogonal quick-win the moment the
  interrupt-fabric datasheet reading is done, independent of the M4 driver work.

---

## See also

- `docs/design-rp2350.md` -- the ARM Cortex-M33 port; its "DEFERRED (b): Hazard3" section
  (`:179-205`) is the seed of this spike and the source of the ~70%-shared claim.
- `docs/design-multicore.md` -- RP2350 hardware facts (ARCHSEL 3.9, SIO 3.1, platform
  timer 3.1.8, core-1 launch 5.3); the SMP pairing.
- `docs/design-riscv-gp-split.md` + `docs/design-cxx-under-mpu.md` -- full-C++ under PMP,
  backend-shared with Hazard3.
- `arch/riscv/chip/esp32c6/` -- the template chip backend (M/U-only, custom controller,
  real UART, PMP on silicon).
- `docs/reference/porting.md` -- the `rv32imac` arch / PMP backend seam.
