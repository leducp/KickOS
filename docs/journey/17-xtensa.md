<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 17. Xtensa: the same walk on an ESP32

This chapter walks the port to the ESP32 (the `esp32-wroom` board), whose cores are Xtensa LX6.
It follows the stops of the reference walk on the Cortex-M4 ([power-on](01-power-on.md) and
[the first syscall](02-first-syscall.md)) and shows only what Xtensa does differently and why.
Two things make this port unlike every other one. Its registers are *windowed*: a function call
slides a window over a large register file instead of saving registers on the stack, and the
context switch has to deal with that. And the core has no privilege levels and the chip offers
KickOS no memory protection it uses, so this is the one port where the "system call" is an
ordinary function call. By the end you will have seen how the windows are spilled on a switch,
how the interrupt levels and the interrupt matrix work, the timer that matches only on equality,
and what KickOS still enforces when the hardware enforces nothing.

The concepts are in the Book: [Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md)
(its startup section explains why the Xtensa startup is the largest assembly file in the tree),
[Interrupts and traps](../book/interrupts-and-traps.md),
[Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md) and
[The time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md). The arch seam is in
[Reference: porting](../reference/porting.md).

## How this port differs, in one table

| Stop | Cortex-M4 (reference) | Xtensa LX6 (ESP32) |
|---|---|---|
| Reset | the CPU reads a vector table | the chip's ROM loads the image into RAM and jumps to `_start` |
| Registers | 16, saved on the stack by calls | 64 physical, seen through a sliding window of 16 |
| Mandatory assembly | reset and the switch | also six window overflow and underflow handlers at fixed offsets |
| Critical section | BASEPRI | `rsil` raises the interrupt level in PS |
| Syscall | `svc` | a plain function call: no privilege to cross |
| Switch from a thread | PendSV | immediate, in the caller: spill the windows, swap, `retw` |
| Switch from an interrupt | PendSV | at the end of the level-1 interrupt exit |
| Timer | SysTick | `CCOMPARE0`, an equality match against the cycle counter |
| Interrupt controller | NVIC | the interrupt matrix plus 32 CPU interrupts at fixed levels |
| Protection | MPU regions | none driven: every thread can reach every address |

## The image, without an emulator

No emulator in this tree models the ESP32, so nothing here was run under gdb. The static Try-its
read the linked image with the toolchain's `xtensa-esp32-elf-objdump` and `xtensa-esp32-elf-nm`;
they were checked on a build of the `esp32-wroom` preset (`cmake --preset esp32-wroom -B
build/esp32-wroom`, then `cmake --build build/esp32-wroom --target hello`), and the addresses below
come from that build. The live ones would need a JTAG debugger on the board, for which the tree
has no recipe (the flashing guide covers [programming over esptool](../flashing.md)); they say
what to expect, not what a run printed. The `esp32-wroom` base configuration runs one core; the `-smp` variant runs
both, and [the multicore chapter](11-multicore.md) covers what that adds.

### Stop 1: reset, and the ROM that loads the image
**Where:** `arch/xtensa/chip/esp32/esp32.ld`, `ENTRY(_start)`, `.vectors`;
`arch/xtensa/chip/esp32/startup.S`, `_start`, `CORE_PROLOGUE`;
`arch/xtensa/chip/esp32/chip_esp32.cc`, `Reset_Handler`

On the Cortex-M4 the CPU fetches its first instruction from flash through the vector table. The ESP32
starts in code of its own, the mask ROM. The ROM reads the image header at flash offset `0x1000`,
copies each segment of the image to the RAM address the linker gave it, and jumps to the entry
point, which `esp32.ld` sets to `_start`. KickOS links everything into internal RAM: code into
`IRAM` at `0x40080000`, data into `DRAM` at `0x3FFB0000`. There is no second-stage bootloader and
no flash cache to set up.

`_start` then does what a windowed core needs before its first function call (`CORE_PROLOGUE`).
It turns the window machinery off in PS (the processor state register), seats the window state
(`WindowBase` = 0, `WindowStart` = one live window), sets the stack pointer `a1`, points `VECBASE`
at KickOS's vector table, and only then writes the PS the C code runs with, still masking every
interrupt. The first call is `call4 Reset_Handler`. `Reset_Handler` copies `.data` (a loop that
does nothing here, the ROM having placed it already) and zeroes `.bss`, then `kickos_crt_tail`
runs the constructors, calls `arch_init`, and enters `kmain`, exactly as on the reference walk.

**Try it:** `xtensa-esp32-elf-objdump -d hello`, find `<_start>:`. You should see the sequence
above, with `rsync` after each special-register write:

```
40081e18:	f01c      	movi.n	a0, 31
40081e1a:	13e600        	wsr.ps	a0
...
40081e22:	134800        	wsr.windowbase	a0
...
40081e2a:	134900        	wsr.windowstart	a0
...
40081e36:	13e700        	wsr.vecbase	a0
...
40081e45:	007595        	call4	400825a0 <Reset_Handler>
```

**Why, and the contract:** [Book: context switching, "Startup"](../book/context-switching-and-the-silicon-contract.md),
[Reference: boards](../reference/boards.md)

### Stop 2: register windows, and the six handlers they need
**Where:** `arch/xtensa/chip/esp32/startup.S`, `_WindowOverflow4`, `_WindowUnderflow4`,
`_WindowOverflow8`, `_WindowUnderflow8`, `_WindowOverflow12`, `_WindowUnderflow12`

This is the idea the rest of the chapter rests on. The LX6 has 64 physical address registers
(`AR`), but an instruction only ever names 16 of them, `a0` to `a15`. Which 16 is set by
`WindowBase`. A call instruction `call4`, `call8` or `call12` records a rotation of 4, 8 or 12
registers, and the callee's `entry` instruction applies it and allocates the callee's stack frame:
the caller's `a4`..`a7` (for `call4`) become the callee's `a0`..`a3`. So arguments are not copied,
and most calls save nothing on the stack. `retw` rotates back.

When the calls go deep enough, the window comes back around onto registers an older caller still
owns. The CPU then raises a *window overflow* exception, and a handler spills that caller's
registers to a fixed place on the caller's stack (the *base save area*, the 16 bytes just below a
frame's stack pointer). On the way back up, `retw` into a frame whose registers were spilled
raises a *window underflow*, and a handler reloads them. These six handlers must sit at fixed
offsets from `VECBASE`, which is why the vector table is part of KickOS and not optional. The
`WindowStart` register has one bit per group of four registers, set where a live frame begins.

For the kernel, two consequences follow. A thread's state is not just "the registers plus the
stack": part of its call chain may still be sitting in the register file, and the next thread will
overwrite it. And because the saved values land in each frame's base save area on the thread's own
stack, the switch can push them there by forcing the overflows, then rebuild everything lazily
through underflows. Stop 9 shows this.

**Try it:** `xtensa-esp32-elf-nm -n hello | head -20`. The handlers start the IRAM, 64 bytes
apart:

```
40080000 t _WindowOverflow4
40080040 t _WindowUnderflow4
40080080 t _WindowOverflow8
400800c0 t _WindowUnderflow8
40080100 t _WindowOverflow12
40080140 t _WindowUnderflow12
40080180 t _Level2InterruptVector
...
40080340 t _UserExceptionVector
```

**Why, and the contract:** [Book: context switching](../book/context-switching-and-the-silicon-contract.md)

### Stop 3: the chip comes up
**Where:** `arch/xtensa/chip/esp32/chip_esp32.cc`, `arch_init`, `wdt_disable`,
`clock_init_240mhz`, `timg_clock_init`, `uart0_irq_setup`; `arch/xtensa/lx6/arch_xtensa.cc`,
`kickos_lx6_init`, `atomctl_seat_or_refuse`

The ROM leaves three watchdogs armed, and each would reset the chip within seconds, so
`wdt_disable` comes first. Then the CPU clock goes to 240 MHz, and the TIMG0 timer that will be
the clock (Stop 6) starts. `kickos_lx6_init` sets up the core: every CPU interrupt disabled
(`INTENABLE` = 0), the FPU enabled for every thread (`CPENABLE`, once per core), and `ATOMCTL`, a
register that decides whether the atomic compare-and-swap instruction `s32c1i` really locks the bus
or only works inside one core. KickOS's kernel lock is built on `s32c1i`, so the boot refuses to
continue if `ATOMCTL` cannot be set to the bus-locking mode. Finally `uart0_irq_setup` routes the
console UART's interrupt (Stop 5).

**Why, and the contract:** [Reference: porting, "The critical section"](../reference/porting.md)

### Stop 4: interrupt levels and the critical section
**Where:** `arch/xtensa/lx6/include/kickos/arch/irq_inline.h`, `arch_irq_save`,
`arch_irq_restore`, `KICKOS_LX6_IRQ_LOCK_LEVEL`; `arch/xtensa/chip/esp32/startup.S`,
`_UserExceptionVector`, `_Level2InterruptVector`

Every Xtensa CPU interrupt has a fixed priority *level* from 1 to 7 (7 is the non-maskable one).
PS holds `INTLEVEL`: interrupts at or below it are masked. The critical section is one
instruction, `rsil`, which returns the old PS and raises `INTLEVEL` to 3. Level-1 interrupts are
the only ones KickOS takes; the vectors for levels 2 to 5, debug and NMI just halt. So level 3 masks
everything KickOS handles, the way BASEPRI does on the Cortex-M4, and restoring is writing the saved PS
back.

A level-1 interrupt does not have a vector of its own. It is delivered as an *exception* with
cause 4, through the same vector as every other exception that happens with `PS.UM` set:

```asm
_UserExceptionVector:
    wsr.excsave1 a0               /* free a0 (original a0 preserved in EXCSAVE1) */
    rsr.exccause a0
    beqi    a0, EXCCAUSE_LEVEL1INTERRUPT, 1f
    j       _kickos_lx6_fault     /* unhandled synchronous exception -> tag + halt */
1:  j       _kickos_int_level1
```

A name to beware of: `PS.UM` is *user vector mode*. It only chooses which vector an exception goes
to (`UserExceptionVector` or `KernelExceptionVector`); it does not make code unprivileged. KickOS
runs the kernel and the threads with it set. While an exception is being handled, `PS.EXCM` is set:
it masks interrupts up to level 3 (the LX6's `EXCM_LEVEL`) and sends a nested exception to
`KernelExceptionVector`, which halts.

**Try it:** `xtensa-esp32-elf-objdump -d hello | grep -A4 '<_UserExceptionVector>:'` shows the
four instructions above at `0x40080340`.

**Why, and the contract:** [Reference: invariants, `irqlock-nesting-safe`, `device-irq-in-maskable-band`](../reference/invariants.md)

### Stop 5: the interrupt matrix and the software controller
**Where:** `arch/xtensa/lx6/arch_xtensa.cc`, `kickos_lx6_dispatch_l1`, `kickos_lx6_bind_dev_int`,
`arch_irq_mask`, `arch_irq_unmask`, `arch_irq_inject`; `arch/xtensa/chip/esp32/chip_esp32.cc`,
`kickos_lx6_dispatch_dev`

Each LX6 core has 32 CPU interrupts, and a register, `INTERRUPT`, that says which are pending.
Peripherals are not wired to them directly. The ESP32's *interrupt matrix* lets software route
any of its peripheral sources to any CPU interrupt of either core. KickOS routes the console UART
(`uart0_irq_setup`) and records the binding with `kickos_lx6_bind_dev_int`. When a level-1
interrupt arrives, `kickos_lx6_dispatch_l1` reads `INTERRUPT & INTENABLE`, and for each pending
CPU interrupt calls the right service: the timer (CPU interrupt 6), the software-raised lines
(7), or
the chip's demultiplexer `kickos_lx6_dispatch_dev`, which reads the UART's own status register
and raises the kernel's logical line.

The kernel's lines are a software controller, as on x86_64 and partly on RX: a mask cell and a
pending cell per line in memory. A device line additionally masks its CPU interrupt in
`INTENABLE`, because a level-triggered source keeps asserting until something masks it. Only CPU
interrupt 7 can be set from software (`INTSET`), so every injected line is recorded in memory and
rung through interrupt 7. That interrupt is enabled just before each ring and disabled by the
dispatcher, because the ROM boots with it pending.

**Why, and the contract:** [Book: a masked interrupt is latched](../book/a-masked-interrupt-is-latched-not-lost.md),
[the interrupt chapter](07-interrupt-to-a-driver.md)

### Stop 6: the timer that must not be late
**Where:** `arch/xtensa/lx6/arch_xtensa.cc`, `arch_timer_arm`, `arch_timer_disarm`,
`arch/xtensa/chip/esp32/chip_esp32.cc`, `arch_clock_now`, `timg_ticks`

Every LX6 core has `CCOUNT`, a 32-bit counter that increments every cycle, and `CCOMPARE0`, a
register that raises CPU interrupt 6 when `CCOUNT` becomes EQUAL to it. Equal, not "at or past".
If the compare value is written after the counter has already passed it, nothing happens until the
counter wraps around, about 18 seconds later at 240 MHz. A few instructions between reading
`CCOUNT` and writing `CCOMPARE0` are enough to cause that for a very short delay. So
`arch_timer_arm` writes, reads `CCOUNT` again, and if the compare value is already behind, doubles
its margin and tries again:

```cpp
    while (true)
    {
        uint32_t const cmp = rd_ccount() + margin;
        __asm volatile("wsr.ccompare0 %0; rsync" ::"a"(cmp) : "memory");
        if (static_cast<int32_t>(rd_ccount() - cmp) < 0)
        {
            break;
        }
        if (margin > (0xFFFFFFFFu / 2u))
        {
            break; // cannot widen further; the wrap is the deadline either way
        }
        margin = margin * 2u;
    }
```

The signed difference makes the test correct across a wrap. The match stays pending until the
next write to `CCOMPARE0`, so the interrupt handler masks the line in `INTENABLE` and the next arm
clears it.

The clock is a separate question. `CCOUNT` is per core, is not synchronised between the two
cores, and its wraps must be observed. The ESP32 chip file therefore supplies `arch_clock_now`
from the TIMG0 timer, a 64-bit counter at 40 MHz shared by both cores. Reading it means
writing an update register to latch the count and reading the two halves; `timg_ticks` reads
the high half twice to catch the other core latching in between.

**Try it:** with a debugger on the board, `break arch_timer_arm` and step through the loop: print
`$ccount` before the `wsr.ccompare0` and the compare value after, and the difference should be the
margin. Then run with a very small sleep (one microsecond) and count how often the loop doubles.
<!-- unverified: no ESP32 emulator, and the tree has no JTAG recipe -->

**Why, and the contract:** [Book: the time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md),
[the sleep chapter](03-sleep.md)

### Stop 7: entering the first thread
**Where:** `arch/xtensa/lx6/arch_xtensa.cc`, `arch_context_init`;
`arch/xtensa/lx6/include/kickos/arch/xtensa_frame.h`; `arch/xtensa/lx6/switch.S`,
`arch_start`, `_thread_trampoline`; `arch/xtensa/chip/esp32/startup.S`, `_kickos_lx6_irq_restore`

A thread can be resumed in two ways on this port (Stop 9), and a new thread uses the
interrupt one. `arch_context_init` fabricates a 256-byte interrupt frame (`F_SIZE`, laid out in
`xtensa_frame.h`) at the top of the thread's stack. Its PC is `_thread_trampoline`, and its PS has
`EXCM` set (so restoring PS cannot be interrupted halfway), `WOE` (window exceptions on), `UM`, and
`CALLINC = 1`, which tells the next `entry` instruction to behave as if it had been reached by a
`call4`. The entry function and its argument are placed in the frame's `a6` and `a7`.

`arch_start` spills the boot code's windows (below), then jumps to `_kickos_lx6_irq_restore`, the
end of the interrupt handler. It reloads every register and returns with `rfe` (return from
exception) into `_thread_trampoline`:

```asm
_thread_trampoline:
    entry   a1, 32                /* open a real CALL4 frame: a2=entry, a3=arg */
    mov     a10, a3               /* arg -> first C argument (call8 window) */
    callx8  a2                    /* entry(arg) */
    call8   kickos_thread_return  /* noreturn */
```

The `entry` rotates the window by 4, so the frame's `a6` and `a7` become `a2` and `a3`. That makes
the trampoline a real windowed frame with a valid base save area. The comment above it in
`switch.S` explains what goes wrong without it: a long chain of calls that wraps the window all the
way around would spill this frame through garbage linkage into data RAM.

Note what the frame does not carry: a privilege bit. `arch_context_init` receives `privileged` and
discards it. There is nothing to put it in.

**Try it:** `xtensa-esp32-elf-objdump -d hello | grep -A5 '<_thread_trampoline>:'` shows the four
instructions above (`call8` is at `0x40082b8b` in this build). Live: `break _thread_trampoline`
and read `$a2` and `$a3` after `entry`: the entry function and its argument.
<!-- unverified (the live half): no ESP32 emulator -->

**Why, and the contract:** [Reference: invariants, `switch-frame-matches-init`](../reference/invariants.md)

### Stop 8: the "syscall" is a function call
**Where:** `arch/xtensa/lx6/arch_xtensa.cc`, `arch_syscall`, `arch_syscall64`;
`kernel/syscall/syscall.cc`, `syscall_dispatch`

On the Cortex-M4 `svc` exists to cross from unprivileged to privileged. The LX6 has no privilege levels
to cross, so the syscall is the dispatcher, called directly:

```cpp
uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    return static_cast<uintptr_t>(syscall_dispatch(nr, a0, a1, a2, a3));
}
```

The rest of the kernel cannot tell the difference. `syscall_dispatch` decodes the number, resolves
handles and validates pointers exactly as on every other port, runs in thread context on the
caller's stack (so it may block), and returns the result in a register. The contract that dispatch
runs privileged, in thread context, on the caller's own continuation is met trivially. What is
lost is the barrier: nothing stops application code from calling kernel functions directly, or
from writing kernel memory (Stop 10). The register IPC fast path is also absent here: it seats a
reply into a saved register frame, and a plain call has none.

**Try it:** `xtensa-esp32-elf-objdump -d hello | grep -A8 '<arch_syscall>:'`:

```
400829e0 <arch_syscall>:
400829e0:	004136        	entry	a1, 32
400829e3:	02ad      	mov.n	a10, a2
...
400829ed:	03b565        	call8	40086544 <syscall_dispatch>
400829f0:	0a2d      	mov.n	a2, a10
400829f2:	f01d      	retw.n
```

**Why, and the contract:** [Book: the syscall path](../book/the-syscall-path-trap-dispatch-return.md),
[Reference: invariants, `syscall-in-priv-thread-context`](../reference/invariants.md)

### Stop 9: the context switch, and the window spill
**Where:** `arch/xtensa/lx6/arch_xtensa.cc`, `arch_switch`; `arch/xtensa/lx6/switch.S`,
`xtensa_switch`, `SPILL_ALL_WINDOWS`; `arch/xtensa/chip/esp32/startup.S`, `_kickos_int_level1`,
`_kickos_lx6_irq_restore`; `arch/xtensa/lx6/include/kickos/arch/context.h`,
`KICKOS_RESUME_COOP`, `KICKOS_RESUME_IRQ`

The Cortex-M4 always defers the switch to PendSV. The LX6 has no such interrupt, and it switches at once
when it can:

```cpp
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    uint32_t const core = arch_cpu_id();
    if (g_isr_depth[core] != 0)
    {
        // The physical swap happens at level-1 interrupt exit (startup.S
        // _kickos_int_level1), which saves g_arch_current in the interrupt-frame format.
        g_arch_next[core] = to;
        g_arch_switch_pending[core].store(1);
        return;
    }
    g_arch_current[core] = to;
    xtensa_switch(from, to);
    // ...
}
```

**From a thread** (a blocking call), `xtensa_switch` saves only three words: the stack pointer,
PS, and `a0`, the return address, whose top two bits record the call size `retw` will need. Then
it runs `SPILL_ALL_WINDOWS`. That macro rotates the window a full turn around the 64 registers
(`rotw 3`, three more times, then `rotw 4`: 16 steps of 4) and at each step touches `a12`. Every
time `a12` falls in a register an older frame still owns, the CPU takes a window overflow and the
handler writes that frame to its base save area on the outgoing thread's stack. After the turn, the
register file holds only the current frame. The thread's whole call chain is now on its own
stack, and three words are enough to find it again. To resume such a thread (`KICKOS_RESUME_COOP`),
the switcher loads its sp, PS and `a0` and executes `retw`; the underflow handlers reload each
caller as the thread returns up its chain.

**From an interrupt** (the timer wakes a higher-priority thread), the level-1 entry has already
saved a full 256-byte frame on the interrupted thread's stack: `a0` to `a15`, the shift amount
register, the three zero-overhead loop registers, and the FPU registers. It then spills that
thread's windows too, so its chain is on its stack. At the end of the interrupt, if a switch is
pending, it records the frame as the thread's `sp`, marks it `KICKOS_RESUME_IRQ`, and resumes the
incoming thread by its own kind: `retw` for a COOP thread, `rfe` through
`_kickos_lx6_irq_restore` for an IRQ one. A cooperative switch to a preempted thread uses the same
`rfe` path, so every combination works.

The FPU follows the same split. The interrupt frame saves `f0` to `f15` and the FPU control and
status registers. The cooperative switch saves none, relying on the calling convention: the FP
registers are caller-saved, so a thread that blocks in a function call has nothing live in them.

**Try it:** `xtensa-esp32-elf-objdump -d hello`, find `<xtensa_switch>:`, and count the
`and a12, a12, a12` / `rotw` pairs: five of each. Live: `break xtensa_switch`, `p/x $windowstart`
before and after the spill; after it, exactly one bit should be set.
<!-- unverified (the live half): no ESP32 emulator -->

**Why, and the contract:** [Book: context switching](../book/context-switching-and-the-silicon-contract.md),
[Reference: invariants, `arch-switch-may-defer`, `deferred-switch-lowest-band`, `fp-xtensa-caller-saved`](../reference/invariants.md)

### Stop 10: protection, or its absence
**Where:** `platform/esp32/chip.yaml`, `protection`; `arch/xtensa/lx6/arch_xtensa.cc`,
`arch_mpu_apply`, `kickos_arch_mpu_commit`, `kickos_lx6_fault_report`;
`arch/xtensa/chip/esp32/startup.S`, `_kickos_lx6_fault`

The ESP32 chip file says it plainly (its "chapter 4" is a chapter of the ESP32 technical
reference manual):

```yaml
# Every thread runs at the kernel's one privilege level, and the PID-keyed MPU of chapter 4 is
# not driven.
protection: { unit: none, privilege: false }
```

The LX6 core as configured in the ESP32 has no rings. The chip has a process-ID based memory
protection scheme in its reference manual, but KickOS does not program it. So on this board the
three axes of privilege from the Book come apart. There is no CPU mode (axis 1). There is no memory
posture (axis 2): `arch_mpu_apply` and `kickos_arch_mpu_commit` are empty, and the build has
`KICKOS_MEMORY_ENFORCED` at 0. Authority (axis 3) is intact, because it is kernel logic: a thread
still needs a capability to touch a kernel object, `syscall_dispatch` still resolves every handle
and checks every right, and a thread without the authority to spawn or reboot is still refused.
What the board cannot do is stop a buggy or hostile thread from bypassing all of that by writing
kernel memory directly.

Faults show the same limit. On the Cortex-M4, a thread's own bad access kills that thread and its task
while the rest of the system runs on. The LX6 port does not implement the fault-isolation seam
(`arch_fault_is_user_thread`), so any unhandled exception goes to `_kickos_lx6_fault`, which reads
the cause, the faulting address and the PC and calls `kickos_lx6_fault_report`. It prints
`=== XTENSA EXCEPTION (load prohibited) ===` or the like, with `EXCCAUSE`, `EXCVADDR` and `EPC1`,
and ends the system.

**Try it:** flash the `fault` app (`user/apps/common/fault`), whose `main` executes an illegal
instruction, and read the console: the report should say `=== XTENSA EXCEPTION (illegal
instruction) ===` with `EXCCAUSE=0x0`, and the board stops.
<!-- unverified: no ESP32 emulator, and no capture of that run is in this tree -->

**Why, and the contract:** [Book: privilege is three axes](../book/privilege-is-three-axes-not-one-bit.md),
[Book: peripheral isolation and the hardware ceiling](../book/peripheral-isolation-and-the-hardware-ceiling.md),
[Reference: porting, "Fault-isolation contract"](../reference/porting.md)

## Where to next

- [18. A real system: an SPI bus service and its client](18-a-real-system.md): the capstone. An SPI driver in user space on the K64F
  and the client that calls it, where the mechanisms of chapters 2 to 9 work together.
- [The multicore chapter](11-multicore.md): the ESP32's second core, the cross-core doorbell, and
  the kernel lock on `s32c1i`.
- [15. x86_64](15-x86_64.md) and [16. RX](16-rx.md): two ports where privilege and protection are
  real.
- [The fault chapter](09-fault-and-kill.md): what a port with fault isolation does instead.

## Exercises

1. In `SPILL_ALL_WINDOWS` the rotations are 3, 3, 3, 3 and 4. Each `rotw` moves the window by
   that many groups of four registers. Check that the total is a full turn of 64 registers, and
   explain why the last step is 4 and not 3.
2. `arch_timer_arm` starts with a margin of at least `CCOMPARE_MIN_CYCLES` (64). If the code between
   the two `rd_ccount()` calls took 100 cycles, how many passes would the loop take for a 1-cycle
   delay? Predict, then check it on the board with a counter.
3. A thread on this board calls `kos_sem_post` with a handle it never received. Follow the call
   from `arch_syscall` into `syscall_dispatch` and find where it is refused. Then say what the same
   thread could do instead, on this board, that the Cortex-M4 would stop.
