<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# RP2350 Hazard3 (RISC-V) port -- feasibility / design spike

> **Status: EXPLORATORY**
**EXPLORATORY -- NOT A CONTRACT. NO IMPLEMENTATION.** A feasibility study for running
KickOS on the RP2350's **RISC-V Hazard3** cores, as a sibling of the existing ARM
Cortex-M33 port (`arch/arm/chip/rp2350/`, `docs/design-rp2350.md`). Register facts that
would be clean-room from the datasheet are marked **[to-verify: RP-008373 / Hazard3
docs]** where I lack a primary source in-repo -- the datasheet is not in the tree, and
this doc invents no addresses.

The hook: one die carries BOTH a dual Cortex-M33 pair and a dual Hazard3 pair, ISA
selected at boot by OTP `ARCHSEL`, and the `rv32imac` arch layer is already silicon-proven
(ESP32-C6 PMP enforcement on hardware; qemu-virt). So the SAME board can boot KickOS as an
ARM MPU kernel or a RISC-V PMP kernel.

## 1. Verdict

**Feasible, and structurally cheaper than the first RISC-V port was -- but NOT a weekend
spike. Milestone-class-small: a strong week of in-tree work plus a silicon bring-up tail,
gated on one crux.** It is cheap because the `rv32imac` arch layer is already ISA-generic
and chip-parameterised (a Hazard3 port is a new CHIP backend, not a new arch), the
core-agnostic half of the SoC is already written and reasoned-about in the ARM backend,
and the ESP32-C6 taught the two hardest lessons Hazard3 shares with it: an M/U-only core
with no S-mode, and a non-standard external-interrupt controller behind chip hooks.

**The crux risk (section 4.3): the Hazard3 interrupt + timer + software-IPI fabric.** The
`rv32imac` scheduler rests on THREE core-local facilities the chip must provide: (a) a
machine software interrupt (`g_clint_msip`) to pend the deferred context switch; (b) a
64-bit machine timer (`mtime`/`mtimecmp`) for the tickless clock; (c) an external
interrupt path for device IRQs. On the C6 all three came from a memory-mapped CLINT +
PLIC. Whether they map onto Hazard3 as cleanly is the SINGLE LARGEST UNKNOWN and gates the
effort estimate. The arch layer already isolates all three behind a pointer and chip hooks
with fallback TUs, so the port is a chip-backend fill and not arch-layer surgery.

## 3. Boot model

- **3.1 ARCHSEL.** `ARCHSEL` is sampled ONLY at reset, so a watchdog reset can flip
  architecture in software: a "boot the other ISA" demo needs no re-flash. Mixed
  Arm+RISC-V is out of scope. Practical selection paths **[to-verify: RP-008373 3.9.x +
  picotool docs]**: confirm the exact NON-PERMANENT selection mechanism (picotool or
  bootrom rather than burning OTP fuses) before committing to a demo flow.
- **3.2 Entry convention.** The RISC-V entry convention differs from ARM's "SP@[base+0],
  reset PC@[base+4] vector table" **[to-verify: RP-008373 5.9.3.4]**: a RISC-V image is
  entered at a plain entry point, so IMAGE_DEF likely needs an explicit `ENTRY_POINT`
  item. Different `startup.S` prologue, and the CPU field in `image_type_flags` is
  RISC-V(1) instead of ARM(0).
- **3.3 XIP-vs-SRAM is open.** Either way the PMP NAPOT code region must stay
  naturally-aligned pow2, and the `.appdata`/gp window needs re-homing if the data LMA
  moves to flash.

## 4. Arch reuse: the interrupt / timer / IPI crux

- **4.1 Software interrupt. Hazard3 unknown [to-verify: RP-008373 + Hazard3 docs]:**
  Hazard3 supports the standard machine software interrupt, but the RP2350 mechanism to
  SET it per-core is the question. Either a CLINT-like MSIP register, in which case
  `g_clint_msip` is just a different address and there is zero arch change, or assertion
  via a custom CSR or SIO doorbell, in which case the arch's "write a word through a
  pointer" contract must widen to a chip hook (`arch_rv_pend_switch`, fallback TU keeping
  the MMIO write for C6/virt). **This is the most likely place the arch layer needs a
  touch.**
- **4.2 Machine timer. Hazard3 unknown [to-verify: RP-008373 3.1.8]:** a RISC-V platform
  timer DOES exist (DS 3.1.8), so the open questions are its base address, tick
  rate/source and whether it is per-core. Lower risk than 4.1. The ARM-side 64-bit TIMER0
  (`0x400b0000`) is core-agnostic MMIO, so `arch_clock_now` can reuse the proven ARM body
  and leave only the compare interrupt to the RISC-V timer.
- **4.3 External-interrupt path. Hazard3 [to-verify: Hazard3 docs + RP-008373]:** Hazard3
  has its **own external interrupt controller integrated into the core**, exposed through
  **custom Hazard3 CSRs** (the Hazard3 IRQ array: per-IRQ enable/pending/priority CSRs,
  e.g. the `meiea`/`meipa`/`meifa`/`meie`-family and a `meinext`-style claim -- **exact
  CSR names/encodings to-verify**). It is **NOT a memory-mapped standard PLIC.** This is
  the crux:
  - **Good news:** the arch layer already assumes a non-standard controller reached
    through those hooks, which is exactly why the C6 fit without arch surgery. The demux
    in `switch.S` keys on `mcause`; Hazard3's external interrupt is the standard `mcause`
    machine-external cause **[to-verify it does not use a C6-style custom mcause=ID
    scheme]**.
  - **Risk:** if the claim/EOI is done via a CSR read, the trap-entry asm may need a small
    new branch, so a `switch.S` edit rather than pure C hooks. The M/U-only plus
    custom-controller combination is precisely the C6 pattern, so the SHAPE of the work is
    known; only the register/CSR facts are new.
  - **Software IPI for SMP later:** the RP2350 SIO doorbells and FIFOs
    (`docs/design-m7-smp.md`) are the cross-core notify primitive and are core-agnostic,
    an M6 concern and not needed for single-core bring-up.
- **4.4 Privilege + extensions.** Hazard3 on RP2350 implements M and U modes **[to-verify
  Hazard3 has_user is enabled on RP2350]**, and like the C6 it is expected to be M/U-only
  with no S-mode, so the SSIP inject channel is a no-op needing the chip override. Bring
  up on plain rv32imac, which reuses the exact soft-float multilib the C6 and virt use for
  zero toolchain risk; treat Hazard3's B extension as a later code-density knob.

## 6. PMP enforcement on Hazard3

The lowest-risk part of the port: the 8-entry NAPOT PMP backend is written and
silicon-proven on the C6, and M-mode-bypasses-unlocked-entries is core-independent
privileged-spec behavior. **Does Hazard3 implement PMP? Yes [to-verify count: RP-008373 /
Hazard3 config].** The region count is a synthesis parameter and the backend assumes 8, so
if RP2350-Hazard3 exposes fewer the domain region budget must be re-checked. The C6's
all-ones-NAPOT quirk (why its bootstrap entry uses TOR) needs the same re-verification
here; the TOR bootstrap is the safe already-written path either way.

## 7.4 Flashing path

**picotool understands RISC-V RP2350 images [to-verify picotool version supports
RISC-V]:** the UF2 family ID and the IMAGE_DEF CPU flag distinguish a RISC-V image, and
the BOOTSEL USB path is the same as the ARM flow. The board is always BOOTSEL-recoverable,
so a wrong clock/boot/ARCHSEL config cannot brick it.

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

The in-tree half (shared-SoC header refactor, board descriptor, chip backend skeleton,
linker script, image inspection) is buildable today; the console, scheduler and PMP stages
are silicon-gated and dominated by unknown #1. If the Hazard3 IRQ controller maps cleanly
onto the existing chip-hook seam (as the C6's INTMTX+PLIC did) the tail is short; if it
needs a `switch.S` demux edit or an `arch_switch` pend-hook, add a few days for the arch
touch PLUS re-validating every existing RISC-V board (C6, virt) against the widened seam.

### 8.4 Roadmap fit

A driver-era demonstrator, neither gated by the driver era nor gating it. It pairs with M6
SMP, since Hazard3 has its own dual-core story on the same die, so the per-CPU-seam and BKL
work can be validated on dual-Hazard3 AND dual-M33 from one board; `docs/design-m7-smp.md`
calls RP2350 a rare cross-ISA test vehicle for the same SMP and AMP code paths.

## See also

- `docs/design-rp2350.md` -- the ARM sibling port and the source of the ~70%-shared claim.
- `docs/design-m7-smp.md` -- RP2350 hardware facts (ARCHSEL 3.9, SIO 3.1, platform timer
  3.1.8, core-1 launch 5.3); the SMP pairing.
- `docs/design-riscv-gp-split.md` + `docs/design-cxx-under-mpu.md` -- full-C++ under PMP,
  backend-shared with Hazard3.
- `arch/riscv/chip/esp32c6/` -- the template chip backend.
- `docs/reference/porting.md` -- the `rv32imac` arch / PMP backend seam.
