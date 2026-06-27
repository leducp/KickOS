<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS porting guide

This is the practical companion to `architecture.md`: how a new target implements
the `arch.h` seam. It also records the **M1 ARMv7-M spike** — the design and
feasibility conclusion for the one mechanism the roadmap flagged as make-or-break.

The porting seam is `arch/include/kickos/arch/arch.h` (authoritative). A target
provides two halves:

- an **arch** backend (`arch/<arch>/`, e.g. `arch/arm/armv7m/`) — the ISA-generic
  machinery: context switch, syscall trap, critical section, core timer/clock, NVIC;
- a **chip** backend (`arch/arm/chip/<chip>/`) — the hardware edges: reset/startup
  + vector table, clock tree, UART console, `arch_init`/`arch_shutdown`, the
  linker script (which defines the user-RAM region `__kickos_ram_start/_end`), and
  `SystemCoreClock` (defined in the chip C, not the linker script).

### Adding a board/chip (the four edit points)

1. `cmake/kickos.cmake` — add the board to `kickos_resolve_board` (→ arch) and, if
   it has a brought-up chip, to `kickos_resolve_chip` (→ chip dir name).
2. `cmake/toolchain-arm-none-eabi.cmake` — add the board → `-mcpu/-mfpu` row.
3. `CMakePresets.json` — add a configure + build preset (only for boards that
   actually build/link today).
4. `arch/arm/chip/<chip>/` — the chip sources (`*.cc`, `*.S`, auto-globbed) and a
   linker script named exactly `<chip>.ld` (the app link picks it up by that name).

Status: the **armv7m arch backend exists** (Cortex-M3/M4) and is validated on QEMU
(`qemu` board / `mps2` chip). The K64F **chip** backend (MCG clocks/UART/linker on
real silicon) is the next M1 step.

---

## The ARMv7-M syscall spike (the M1 de-risk)

### The problem

`arch.h` requires (portability-critical contract, quoted from the header):

> the arch MUST run `syscall_dispatch()` in privileged **THREAD** context on the
> calling thread's own continuation — NOT in ISR/handler context. A blocking
> syscall blocks by an ordinary synchronous context switch … `arch_in_isr()` must
> read false during dispatch.

On Cortex-M, `SVC` traps into **handler mode**. If `syscall_dispatch()` ran in the
SVC handler, then during a blocking call:

- `arch_in_isr()` (IPSR ≠ 0) would read **true** — contract violation;
- a blocking `arch_switch()` pends **PendSV**, but PendSV is lower priority than
  the active SVC handler, so it **cannot preempt** it. The switch would be
  deferred until SVC returns — i.e. the syscall would return to the user instead
  of blocking. The kernel's blocking primitives would break.

The roadmap note: *validate this in M1 week one; if infeasible the fallback
(deferred syscall completion) is a core restructure.* **It is feasible.** Design:

### The design (implemented in `arch/arm/armv7m/switch.S`)

Dispatch runs in **privileged thread mode** via an exception-return trampoline.
Two observations make it small:

1. The hardware exception frame that `SVC` stacks **already carries the syscall
   arguments** (`r0`=nr, `r1..r3`=a0..a2, `r12`=a3 — the user stub loads a3 into
   r12 before `svc`) and, in the stacked `LR`, the return address of
   `arch_syscall`'s caller. So the SVC handler only has to rewrite the stacked
   `PC`.
2. **Lowering** privilege in thread mode needs no trap — a plain `MSR CONTROL`
   with `nPRIV=1` suffices. Only *raising* privilege needs the trap.

Flow:

```
user (unprivileged, PSP)          arch_syscall:  ldr r12,[sp]  ; a3 -> stacked r12
                                                 svc #0
      │  hardware stacks {r0..r3,r12,lr,pc,xPSR} on PSP, enters handler mode
      ▼
SVC_Handler (handler mode)        rewrite stacked PC := svc_trampoline
                                  CONTROL.nPRIV = 0            ; trampoline privileged
                                  bx lr (EXC_RETURN thread/PSP)
      │  hardware unstacks -> thread mode, r0..r3/r12 = args, LR = caller-return
      ▼
svc_trampoline (PRIVILEGED THREAD mode, on the thread's own PSP stack)
                                  push {r12,lr}               ; a3 at [sp], save caller-return
                                  bl syscall_dispatch         ; <-- runs in thread mode
                                  ... (may block: see below) ...
                                  CONTROL.nPRIV = 1           ; drop to unprivileged
                                  bx lr                       ; -> arch_syscall's caller, r0=result
```

**Why blocking now works.** `syscall_dispatch` runs in *thread* mode, so
`arch_in_isr()` reads false. A blocking call reaches `arch_switch`, which pends
PendSV. When the kernel's `IrqLock` (BASEPRI) is released, **PendSV — a real
exception, higher priority than thread mode — preempts the trampoline** and
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
  User stacks must be sized for kernel call depth — the M0 model; a separate
  per-thread kernel stack is a later refinement.
- **Privilege is a saved/restored register, not the resting privilege.** A thread
  blocked mid-syscall is *privileged* (the trampoline raised it). PendSV therefore
  **saves the outgoing thread's current `CONTROL.nPRIV`** into its context and
  restores the incoming thread's saved value — so a mid-syscall thread resumes
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

The saved-frame layout (low→high address on PSP):

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
  `PRIO_LOCK_BASEPRI` (0x20); PendSV/SysTick/SVCall sit at 0xE0–0xF0 and device
  IRQs must be configured ≥ 0x30, so the lock masks all of them while leaving a
  future 0x00/0x10 zero-latency band unmaskable. (v6-M/RP2040 will use PRIMASK.)
- **Monotonic clock** = the **DWT cycle counter**, extended to 64-bit in software.
  *Limitation:* a 32-bit wrap (~35 s at 120 MHz) not observed within one period
  is missed; a DWT/timer overflow interrupt is the refinement.
- **One-shot timer** = **SysTick** in a disarm-on-fire (tickless) mode. Deadlines
  beyond the 24-bit range fire early and the kernel re-arms the remainder (a
  harmless extra wake). A dedicated chip compare timer is the refinement (12a).
- **NVIC** backs `arch_irq_mask/unmask/inject`. `inject` drops a raise on a
  masked (disabled) line to match the proven sim semantics; item 11a revisits
  this on real silicon.
- **MPU** — `arch_mpu_apply` is a **no-op on M1** (privilege + SVC only); per-task
  hardware MPU enforcement is M2 (item 12).

---

## The QEMU verification target (`boards`: `qemu`, chip `mps2`)

A runnable armv7m target validates the arch layer on real Cortex-M4:
`qemu-system-arm -M mps2-an386`. The `mps2` chip backend
(`arch/arm/chip/mps2/`) uses **semihosting** for the console (`SYS_WRITEC`) and
exit (`SYS_EXIT_EXTENDED`), so it needs no UART; its linker script maps code at
0x0 and SRAM at 0x20000000.

Two toolchain/QEMU gotchas the chip layer resolves (both documented at their fix
site):
- **picolibc default linker script.** This toolchain (Debian arm-none-eabi) is
  picolibc-based; its spec injects a default `-T picolibc.ld` *unless it sees a
  driver-level `-T`*. A `-Wl,-T` is invisible to that check, so both scripts
  applied and collided at address 0. The app link passes `-T` at the driver
  level (see the `kickos` interface `target_link_options`).
- **QEMU's DWT cycle counter is frozen.** The arch layer's default
  `arch_clock_now` (DWT `CYCCNT`) is `weak`; the `mps2` chip overrides it with
  the semihosting `SYS_CLOCK` (the monotonic clock source is legitimately
  chip-specific). Real silicon uses the DWT default. Caveat: on QEMU the clock
  (host wall-time, 10 ms granularity) and the one-shot timer (SysTick counting
  virtual cycles) are **two uncorrelated timebases**, so sub-10 ms deadlines land
  up to 10 ms late and can cause a bounded re-arm churn until the coarse clock
  advances. This makes the QEMU gate a *functional* check, not a timing-accurate
  one; real silicon runs the clock and the compare off the same source.

## Verification status

The arch backend **cross-compiles clean** for Cortex-M4/M3, and the **spike is
empirically validated on QEMU Cortex-M4**: `ctest --preset qemu` boots `hello`
and asserts the two userspace threads ping-pong — exercising reset → C-runtime →
scheduler start (PendSV first switch) → **SVC-trampoline syscalls from both a
privileged and an unprivileged thread** → SysTick one-shot driving `sleep` →
semaphore block/wake reschedule, all on a real Cortex-M4 core. The #1 M1 risk is
retired.

Next runtime target is the K64F **chip** step (M1 item 10) — the same arch layer,
a real SYSMPU-less bring-up (MCG clocks, UART0, linker with the flash config
field); roadmap acceptance: "UART output matches the sim, minus the
enforced-MPU-fault case."
