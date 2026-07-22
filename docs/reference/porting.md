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
  diagnostic LED, `kdiag_led_*`); both have weak no-op defaults, so a board with
  no known LED just leaves them out.

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
`arch_console_reclaim` body (the generic default is a weak no-op -- a silent
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
board-specific `include/kickos/board_config.h` (preferred over the chip default)
and/or a `<chip>.ld` linker override -- proven on the `stm32f411` pair
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

A chip whose flash boot needs a checksummed second stage (RP2040 boot2) adds a
fifth point: `cmake/<chip>_checksum.py` plus a `boot2.S`/`boot2.ld` in the chip
dir; the build wires the multi-stage boot2 image automatically (keyed on
`${KICKOS_CHIP}`). A chip on a non-ARM ISA additionally needs a new arch backend +
toolchain file -- see `arch/xtensa/` (ESP32) or `arch/rx/` (RX72M) for worked examples.

Status -- five arch backends (**armv7m** Cortex-M3/M4/M4F, **armv6m** Cortex-M0/M0+,
**rxv3** Renesas RX72M, **lx6** Xtensa/ESP32, **rv32imac** RISC-V) across the chips below,
by validation tier:

| Chip | Board | Core | Validation |
|------|-------|------|------------|
| `mps2` | qemu | M4F | QEMU (runnable CI gate) |
| `nrf51` | microbit | M0 | QEMU (runnable CI gate) |
| `virt` | qemu-riscv | RV32IMAC | QEMU (runnable CI gate) |
| `xmc4800` | xmc4800-relax | M4F | **hardware** (LED + USIC VCOM console over the buffered ring) |
| `stm32f411` | f411disco / blackpill | M4F | **hardware** (LED + UART + ping-pong) |
| `stm32f302` | f302nucleo | M4 | **hardware** (LED PB13 + console; RAM-limited selftest) |
| `stm32f103` | bluepill | M3 | **hardware** (RAM-limited selftest; test 11 = 4 K alloc > 10 K LD floor) |
| `rp2040` | picopi | M0+ | **hardware** (selftest over UART0/GP0) |
| `mk64f` | frdmk64f | M4F | **hardware** (revalidated 2026-07-15: full selftest + buffered console ring on silicon); SYSMPU is the M2 enforcement backend |
| `rx72m` | rx72m | RXv3 | **hardware** (selftest + SCI6 console; DPFPU switch) |
| `esp32` | esp32-wroom | Xtensa LX6 | **hardware** (selftest + console, 240 MHz) |
| `esp32c6` | esp32c6-wroom | RV32IMAC | **hardware** (selftest + buffered ring console; first real peripheral IRQ) |
| `sam3x8e` | due | M3 | port proven on silicon (2026-07-09); test unit retired (peripheral-I/O fault) |

Build-only chips are verified by construction (register review + image
inspection); flash to a board to confirm. `apps/blink` is a no-UART LED smoke
test available on every board with a known LED.

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
  enforcement at M2. `arch_mpu_apply` only **stashes** the incoming region set (shared/weak);
  the weak `kickos_arch_mpu_commit` / `kickos_arm_mpu_program` **program the hardware** from the
  PendSV switch epilogue, after the physical swap (the deferred-commit seam,
  `design-mpu-commit-deferred.md`). A chip with a non-PMSAv7 MPU overrides the commit, never
  `arch_mpu_apply` (K64F SYSMPU, RP2350 PMSAv8). `arch_mpu_region_encodable` bounds a grant to
  what the backend can describe.
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
- **QEMU's DWT cycle counter is frozen.** The arch layer's default
  `arch_clock_now` (DWT `CYCCNT`) is `weak`; the `mps2` chip overrides it with
  the semihosting `SYS_CLOCK` (the monotonic clock source is legitimately
  chip-specific). Real silicon uses the DWT default. Caveat: on QEMU the clock
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

Since then the chip layer has been brought up on real hardware: `mk64f` (M1
baseline + M2 SYSMPU + the first unprivileged MMIO drivers) and **`rp2040` --
running on a real Raspberry Pi Pico** (see the RP2040 section).
Remaining M1 chips (F411/F103) reuse the `mps2`/`mk64f`/`rp2040` patterns. Hardware
MPU enforcement is M2.
