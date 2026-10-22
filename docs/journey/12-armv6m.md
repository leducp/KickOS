<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 12. ARMv6-M: the same walk on a Cortex-M0 and M0+

The reference walk ran on a Cortex-M4 (ARMv7-M). This chapter takes the same stops on the
smallest ARM cores KickOS runs on: the Cortex-M0 of the BBC micro:bit (board `microbit`, chip
`nrf51`) and the Cortex-M0+ of the Raspberry Pi Pico (board `picopi`, chip `rp2040`). Both are
**ARMv6-M**, the "baseline" M profile. The design does not change: the switch still happens in
PendSV, the syscall still traps with `svc` and runs its dispatch in privileged thread mode on
the caller's own continuation. What changes is how much of the machine is there to do it with.
By the end you will have seen the five places where that shows: a smaller instruction set, a
critical section with no priority band, a switch frame one word shorter, a privilege bit that
the Cortex-M0 silently ignores, and an MPU that one part has and the other does not.

The concept chapters are [Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md)
(what a switch must preserve) and [Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md)
(why "unprivileged" can mean less than it says). The arch seam is in
[Reference: porting](../reference/porting.md). Read [Power-on](01-power-on.md) and
[The first syscall](02-first-syscall.md) first: this chapter only shows what differs.

## The images we use

QEMU has a `microbit` machine with a real Cortex-M0, and the project's CI runs the `microbit`
preset on it, so every `microbit` stop below runs under gdb. QEMU has no RP2040 model, so the
stops that only the Pico has (the second-stage boot loader and the MPU) are read from the code,
not run.

[Setup](00-setup.md) builds only the `qemu` preset. Build `microbit` the same way:

```
cmake --preset microbit
cmake --build --preset microbit
```

`ctest --test-dir build/microbit -N -V -R 'microbit_hello$'` prints the emulator line. Run it
halted with the gdb server on, as in [Setup](00-setup.md):

```
qemu-system-arm -M microbit -global nrf51-soc.sram-size=32768 -nographic -semihosting \
    -kernel build/microbit/user/apps/common/hello/hello -gdb tcp::1234 -S &
gdb-multiarch build/microbit/user/apps/common/hello/hello \
    -ex 'target remote :1234'
```

The `-global` sets the SRAM size of the 32 KiB nRF51822 variant the chip file describes;
QEMU's default is smaller. The gdb output below is trimmed, and the addresses are from one
build: yours will differ.

Two of the commands print the running thread's context. The kernel keeps it in
`g_arch_current`, an atomic wrapper, so the raw pointer is `g_arch_current.v_._M_b._M_p`.

### Stop 1: the vector table, with no VTOR
**Where:** `arch/arm/chip/nrf51/startup.S`, `g_isr_vector`; `arch/arm/common/core_vectors.inc`,
`KICKOS_CORE_VECTORS_V6M`; `arch/arm/chip/nrf51/chip_nrf51.cc`, `Reset_Handler`

Power-on is the same as on the reference: the core reads word 0 of the vector table into the
main stack pointer (MSP) and word 1 into the program counter. The difference is where the table
can be. A Cortex-M0 has no VTOR (the register that lets software move the table), so the table
must sit at address 0, and the linker script puts `.isr_vector` at the start of flash. The
Cortex-M0+ does have a VTOR, and the RP2040 uses it: its boot ROM runs a 256-byte second stage
(`arch/arm/chip/rp2040/boot2.S`) that sets up the external flash and then points VTOR at the
real table.

The table itself is shorter in meaning. `KICKOS_CORE_VECTORS_V6M` leaves slots 4, 5 and 6 empty:
ARMv6-M has no MemManage, BusFault or UsageFault exception. Every fault is a HardFault (Stop 11).

`Reset_Handler` on the nRF51 is C, as on the reference chip, but it copies `.data` and clears
`.bss` with two plain loops instead of calling `kickos_ranges_init`. Then `kickos_crt_tail` runs
the constructors, calls `arch_init` and enters `kmain`, exactly as in [Power-on](01-power-on.md).

**Try it:** connect, then before `continue`:
```
(gdb) x/4wx 0
0x0:  0x20008000  0x00001be9  0x00001c45  0x00001d79
(gdb) info registers sp pc
sp   0x20008000
pc   0x1be8   <Reset_Handler()>
```
Word 0 is the top of the 32 KiB SRAM, and it is already in `sp`. Word 1 is `Reset_Handler` with
bit 0 set (the Thumb bit), and the core has cleared it into `pc`.

**Why, and the contract:** [Book: Interrupts and traps](../book/interrupts-and-traps.md),
[Reference: porting, the seven edit points](../reference/porting.md)

### Stop 2: exception priorities, two bits wide
**Where:** `arch/arm/armv6m/arch_armv6m.cc`, `kickos_armv6m_init`; `arch/arm/armv6m/regs.h`,
`PRIO_SVCALL`, `PRIO_PENDSV`, `PRIO_SYSTICK`

`arch_init` on the nRF51 does one thing: it calls `kickos_armv6m_init`, which sets the
priorities of the three system exceptions the kernel uses. On the reference these are three
different levels (SVCall and SysTick at `0xE0`, PendSV at `0xF0`, the lowest). ARMv6-M
implements only the top two bits of each priority byte, so there are four levels in total, and
KickOS puts all three exceptions on the lowest one, `0xC0`. Equal priority means none of them
can preempt another: a PendSV requested inside the SVC or SysTick handler waits until that
handler ends, and the hardware then takes it straight away, before going back to the thread
(it "tail-chains" the two). Device interrupts are left at their reset priority, 0, the highest.

The second difference is the access width. The priority registers `SHPR2` and `SHPR3` accept
only whole-word writes on v6-M, so the code reads the word, masks out one byte and writes the
word back. On v7-M a single byte store would do.

**Try it:** `break kickos_armv6m_init`, `continue`, `finish`, then:
```
(gdb) x/2wx 0xE000ED1C
0xe000ed1c:  0xc0000000  0xc0c00000
```
`SHPR2` holds SVCall in its top byte; `SHPR3` holds PendSV and SysTick in its two top bytes.

**Why, and the contract:** [Reference: invariants, `deferred-switch-lowest-band`](../reference/invariants.md)

### Stop 3: the critical section is PRIMASK
**Where:** `arch/arm/armv6m/include/kickos/arch/irq_inline.h`, `arch_irq_save`,
`arch_irq_restore`

On the reference, the kernel lock (`IrqLock`) raises `BASEPRI` to `0x20`. That masks every
interrupt at priority `0x20` or lower in urgency and leaves a band above it that the kernel
never blocks. ARMv6-M has no `BASEPRI`. Its only mask is `PRIMASK`, one bit that blocks every
configurable interrupt at once. So the critical section here masks everything, and there is no
zero-latency band to keep.

The upside is that both halves become two instructions, so this arch takes both of them inline
(the reference keeps the save out of line, because the `BASEPRI` raise needs a nesting test and
two barriers):

```c
static inline __attribute__((always_inline)) arch_irq_state_t arch_irq_save(void)
{
    uint32_t prev;
    __asm volatile("mrs %0, primask" : "=r"(prev));
    __asm volatile("cpsid i" ::: "memory");
    return prev;
}

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    __asm volatile("msr primask, %0" ::"r"(state) : "memory");
}
```

Nesting is handled by the saved word: an inner lock saves 1 and restores 1, so the section
stays masked until the outermost restore writes back the 0 it found.

**Try it:** `break arch_switch`, `continue` (the scheduler calls it with the lock held):
```
(gdb) info registers primask control
primask  0x1
control  0x2
```
On the reference walk the same breakpoint shows `basepri 0x20` and `primask 0x0`.

**Why, and the contract:** [Reference: porting, the critical section](../reference/porting.md),
[Book: A masked interrupt is latched, not lost](../book/a-masked-interrupt-is-latched-not-lost.md)

### Stop 4: the first switch leaves the boot stack
**Where:** `arch/arm/armv6m/switch.S`, `arch_start`, `PendSV_Handler`;
`arch/arm/common/arch_arm_common.cc`, `arch_switch`

`arch_switch` is the same function on both M profiles (it lives in `arch_arm_common.cc`): it
stores the incoming context in `g_arch_next` and sets PendSV pending. `arch_start` does the
same with `g_arch_current = 0`, the "nothing to save" marker, and PendSV skips its save half
when it sees it.

Watch `lr` on entry to PendSV. On exception entry the core loads `lr` with an `EXC_RETURN`
value that says where to return. The very first PendSV interrupts boot code, which runs on the
MSP, so it reads `0xFFFFFFF9` (thread mode, main stack). PendSV never returns there: its
epilogue always loads `0xFFFFFFFD` (thread mode, process stack), and the boot stack is abandoned.

**Try it:** `break arch_start`, `continue`, `break PendSV_Handler`, `continue`:
```
(gdb) p g_arch_current
$2 = {v_ = {_M_b = {_M_p = 0x0 <...>}}}
(gdb) p/x $lr
$4 = 0xfffffff9
```
`continue` again, and later entries read `0xfffffffd`.

**Why, and the contract:** [Book: Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md),
[Reference: porting, context switch and first-thread start](../reference/porting.md)

### Stop 5: the Thumb-1 save, one word shorter
**Where:** `arch/arm/armv6m/switch.S`, `PendSV_Handler`; `arch/arm/armv6m/arch_armv6m.cc`,
`arch_context_init`; `arch/arm/armv6m/include/kickos/arch/context.h`, `struct arch_context`

The job is the same as on the reference: the hardware has already pushed the eight caller-saved
words (`r0`-`r3`, `r12`, `lr`, `pc`, `xPSR`) on the thread's process stack, and PendSV pushes
the callee-saved `r4`-`r11` below them. Two things change.

First, the instructions. On v7-M one `stmdb r0!, {r4-r11, lr}` does it. ARMv6-M executes only
the 16-bit Thumb-1 encodings, whose push, pop and load/store-multiple reach `r0`-`r7` only. So
`r8`-`r11` are copied down into `r4`-`r7` (which have just been saved) and stored in a second
batch:

```
    subs    r0, #F_TRAP_FRAME       /* room for r4-r11 */
    str     r0, [r3]                /* outgoing->sp = base */
    stmia   r0!, {r4-r7}            /* [base..+12] = r4-r7 */
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    stmia   r0!, {r4-r7}            /* [base+16..+28] = r8-r11 */
    mrs     r0, control             /* save current privilege */
    movs    r1, #1
    ands    r0, r1
    str     r0, [r3, #4]            /* outgoing->npriv */
```

The restore runs the same dance backwards. The same subset rules show up all over the file:
no `cbz`, no `bfi`, no `movw`, no `it` blocks; constants and addresses come from `ldr =`
literals, and conditionals are `cmp` plus a branch.

Second, the frame. The reference stores `EXC_RETURN` as a ninth word, because on an FPU core it
records whether the thread has live floating-point state. ARMv6-M has no FPU, so every thread
returns with the same `0xFFFFFFFD`, and the epilogue rebuilds it from a literal instead of
loading it from the frame. The frame is eight words, and a thread that scribbles on its own
saved frame cannot choose the value the exception return uses.

`arch_context_init` fabricates exactly this layout for a new thread: eight zero words for
`r4`-`r11`, then a hardware frame with `r0` = the argument, `lr` = the return stub, `pc` = the
entry, `xPSR` = `0x01000000` (the Thumb bit).

**Try it:** PendSV calls `kickos_arch_mpu_commit` right after it has restored the incoming
thread, so stop there and look at that thread's saved frame. `break *kickos_arch_mpu_commit`,
`continue` twice (the first hit is the first thread):
```
(gdb) p *g_arch_current.v_._M_b._M_p
$2 = {sp = 536893376, npriv = 1, resting_npriv = 1, stack_lo = 536891392, stack_hi = 536893440, kernel_sp = 536874368}
(gdb) x/16wx g_arch_current.v_._M_b._M_p->sp
0x200057c0:  0x00000000  0x00000000  0x00000000  0x00000000
0x200057d0:  0x00000000  0x00000000  0x00000000  0x00000000
0x200057e0:  0x00000000  0x00000000  0x00000000  0x00000000
0x200057f0:  0x00000000  0x0000171b  0x00000104  0x01000000
(gdb) info registers psp
psp  0x200057e0
```
A fresh thread: eight zeros for `r4`-`r11`, then `r0`-`r3` and `r12`, then `lr` =
`kickos_user_thread_return`, `pc` = `ping` (the hello app's first thread), `xPSR`. `psp`
already points 32 bytes up, at the hardware frame the exception return will pop.

**Why, and the contract:** [Reference: invariants, `switch-frame-matches-init`](../reference/invariants.md),
[Book: Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md)
(its armv6m section says what rebuilding `EXC_RETURN` from a literal does and does not protect)

### Stop 6: the syscall trap, and the fifth argument
**Where:** `arch/arm/armv6m/switch.S`, `arch_syscall`, `SVC_Handler`

The trap is the reference's trap. The user stub executes `svc #0`; the hardware stacks the
eight-word frame on the caller's process stack; `SVC_Handler` checks that stack has room
(`PSP_GUARD`), tries the register-IPC fast path for syscall 56, and otherwise rewrites the
stacked `pc` to `svc_trampoline`, clears `CONTROL.nPRIV` and returns from the exception into
the trampoline.

The one ARMv6-M wrinkle is in the stub. A syscall has up to five arguments: the number in `r0`,
three in `r1`-`r3`, and the fourth argument `a3` on the stack, per the calling convention. The
trampoline expects it in `r12`, because `r12` is in the hardware frame and so survives the
trap. On v7-M that is one `ldr r12, [sp]`. Thumb-1 loads only into a low register:

```
arch_syscall:
    push    {r4}                    /* free a low reg (shifts [sp] a3 by +4) */
    ldr     r4, [sp, #4]            /* a3 */
    mov     r12, r4
    pop     {r4}                    /* restore r4 + SP (8B aligned again) */
    svc     #0
    bx      lr
```

**Try it:** `break kickos_app_main` (the hello app's `main`), `continue`, `delete`,
`break SVC_Handler`, `continue`:
```
(gdb) p/x *(unsigned*)($psp)@8
$1 = {0x32, 0x0, 0xdee3, 0x1d, 0xffffffff, 0x15c3, 0x2260, 0x1000000}
(gdb) info symbol *(unsigned*)($psp+20)
kos_send + 19 in section .text
(gdb) info symbol *(unsigned*)($psp+24)
arch_syscall64 + 10 in section .text
```
The stacked frame: `r0` = 50 (the syscall number of `kos_send`, which `main`'s first `kos::print`
makes to its stdout endpoint at index 0), `r1`-`r3` its endpoint, buffer and 29 bytes, the
stacked `lr` is the user caller, the stacked `pc` is the instruction after the `svc`.

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md),
[Reference: porting, the ARMv7-M syscall spike](../reference/porting.md)

### Stop 7: the trampoline always moves to a kernel stack
**Where:** `arch/arm/armv6m/switch.S`, `svc_trampoline`; `arch/Kconfig`, `ARCH_KERNEL_STACKS_MANDATORY`

On the reference, whether the dispatch runs on a separate per-thread kernel stack depends on the
chip: `KICKOS_KERNEL_STACKS` is 1 on an ARMv7-M chip with an MPU
([02, Stop 5](02-first-syscall.md#stop-5-the-trampoline-moves-onto-the-kernel-stack)). ARMv6-M has only one path: the arch selects
`ARCH_KERNEL_STACKS_MANDATORY`, and `svc_trampoline` always moves `sp` to the calling thread's
own kernel block (`ctx.kernel_sp`) before calling `syscall_dispatch`. This holds even on the
micro:bit, which has no MPU to protect that block. There the block buys robustness, not
isolation.

The trampoline lays a four-word header at the top of the block and calls `syscall_dispatch`
with `sp` pointing at it. The header holds `a3` (where the C calling convention expects a fifth
argument), the address to return to, the process stack pointer to come back to, and an
alignment pad. A blocking syscall is frozen by PendSV below this header, so the header survives
the block.

**Try it:** `break kickos_app_main`, `continue`, `delete`, `break *syscall_dispatch`,
`continue`:
```
(gdb) info registers sp
sp   0x200009f0   <kickos::(anonymous namespace)::g_kstacks+1776>
(gdb) p/x g_arch_current.v_._M_b._M_p->kernel_sp
$2 = 0x20000a00
(gdb) x/4wx $sp
0x200009f0:  0xffffffff  0x000015c3  0x20004ef0  0x4b53464c
```
`sp` is 16 bytes under the block top. The header reads: `a3` = 0xffffffff, the timeout, the return address (inside
`kos_send`), the thread's process stack pointer, and the pad word, which holds whatever was
there before.

**Why, and the contract:** [Reference: porting, thread pointer and kernel stacks](../reference/porting.md),
[Book: Thread stacks](../book/thread-stacks-and-the-kiss-tension.md)

### Stop 8: a privilege bit the Cortex-M0 ignores
**Where:** `arch/arm/armv6m/switch.S`, `svc_trampoline`; `platform/nrf51/chip.yaml`,
`protection`; `user/apps/common/ringpriv/`

On the way out, the trampoline drops back to the caller's resting privilege. `r0` and `r1`
carry the 64-bit result and `r2`/`r3` hold the saved value and `CONTROL`, so no register is left
for the `#1` mask that `bics` needs; it clears bit 0 with a shift pair instead:

```
    ldr     r2, =g_arch_current
    ldr     r2, [r2]
    ldr     r2, [r2, #8]            /* ctx.resting_npriv */
    mrs     r3, control
    lsrs    r3, r3, #1
    lsls    r3, r3, #1              /* CONTROL.nPRIV = 0 */
    orrs    r3, r2                  /* CONTROL.nPRIV = resting privilege */
    msr     control, r3
    isb
    bx      lr                      /* -> arch_syscall's caller, r0:r1=result */
```

On a Cortex-M0+ this works as on the reference. On a Cortex-M0 it does nothing. The
"unprivileged/privileged extension" is optional in ARMv6-M; the M0 does not implement it, so
`CONTROL.nPRIV` is not writable and every thread runs privileged whatever the kernel asks. The
write is ignored, not refused: no fault is raised. The code is the same for both cores, because
no compiler macro tells an M0 from an M0+; the difference is stated once, per chip, in the chip
file (`privilege: false` for the nRF51), and the build derives from it that the micro:bit has
no privilege ring and so no fault isolation. The `ringpriv` test checks the outcome each board
claims.

**Try it:** `break arch_start`, `continue`, `break svc_trampoline`, `continue`, then
`tbreak *$lr` and `continue` to land back in the user caller:
```
(gdb) info registers control
control  0x2
(gdb) p g_arch_current.v_._M_b._M_p->resting_npriv
$4 = 1
```
The kernel asked for unprivileged (`resting_npriv` = 1, which would make `control` read `0x3`).
The core kept `nPRIV` at 0: `control` is `0x2`, only the stack-select bit set.

**Why, and the contract:** [Book: Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md),
[Reference: porting, probing the ring](../reference/porting.md),
[Reference: invariants, `npriv-banked-on-switch`](../reference/invariants.md)

### Stop 9: the timer is shared, the clock is not
**Where:** `arch/arm/common/arch_arm_common.cc`, `arch_timer_arm`, `SysTick_Handler`;
`arch/arm/chip/nrf51/chip_nrf51.cc`, `arch_clock_now`; `arch/arm/chip/rp2xxx/chip_rp2xxx.cc`,
`arch_clock_now`

The one-shot timer is SysTick on both M profiles, and the code is the shared
`arch_timer_arm` you met in [Sleep](03-sleep.md). The monotonic clock is where v6-M differs. The
reference can count CPU cycles with the DWT unit; ARMv6-M has no DWT, so `arch_clock_now` is
always a chip function. On the nRF51 under QEMU it asks the host through semihosting
(`SYS_CLOCK`). On the RP2040 it reads the chip's own 64-bit microsecond TIMER, which runs from
the crystal and so does not move when the CPU clock is changed.

**Try it:** `break arch_timer_arm`, `continue`, `finish`:
```
(gdb) x/3wx 0xE000E010
0xe000e010:  0x00000007  0x0061a800  0x0061a6ce
```
SysTick `CTRL` = 7 (enabled, interrupt on, core clock), `LOAD` = 6 400 000 cycles, which is
400 ms at the 16 MHz `SystemCoreClock` that `chip_nrf51.cc` sets, and `VAL` counting down from it.

**Why, and the contract:** [Book: The time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md),
[Reference: porting, clock retune](../reference/porting.md)

### Stop 10: the MPU, or its absence
**Where:** `arch/arm/common/kickos_arch_mpu_commit_default.cc`, `kickos_arch_mpu_commit`;
`arch/arm/common/arch_arm_mpu_pmsav7.cc`, `kickos_arm_mpu_program`;
`arch/arm/armv6m/include/kickos/arch/mpu_encoded.h`; `cmake/chip_generate.cmake`,
`kickos_chip_arm_backend`

The MPU is optional in ARMv6-M, and the two boards sit on either side of that line.

The Cortex-M0 has none. The nRF51 chip file says `unit: none`, the build sets
`KICKOS_HAVE_MPU` to 0, and the commit PendSV calls after every switch compiles to an empty
function. Threads are separated by nothing but their own good behaviour, which is what "no
MPU, no privilege ring" means.

The RP2040's Cortex-M0+ has an 8-region MPU. Its register map is the ARMv7-M one (region
number, base, size-and-attributes), so the chip file names `unit: pmsav6` and
`kickos_chip_arm_backend` maps that to the same `PMSAV7` backend the reference uses. The commit
and the shared writer `kickos_arm_mpu_program` run unchanged: rewrite only the descriptors
whose words changed, each one disabled before its base moves. The rules a grant must meet are
the v7 ones too: a region is a power of two in size, aligned to its size, and where regions
overlap the highest-numbered one decides (`ARCH_MPU_OVERLAP_HIGHER`).

**Try it:** on the micro:bit, `break *kickos_arch_mpu_commit`, `continue`:
```
(gdb) x/i $pc
=> 0x22de <kickos_arch_mpu_commit()>:  bx  lr
```
The whole commit is one return.
<!-- unverified: the RP2040 PMSAv6 commit; QEMU has no RP2040 model, so only the micro:bit's empty commit runs here -->

**Why, and the contract:** [Book: Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Reference: invariants, `mpu-apply-on-every-switch-in` and `mpu-commit-writes-what-changed`](../reference/invariants.md)

### Stop 11: every fault is a HardFault
**Where:** `arch/arm/armv6m/arch_armv6m.cc`, `HardFault_Handler`, `kickos_armv6m_fault_report`;
`arch/arm/armv6m/arch_armv6m_fault.cc`, `arch_fault_is_user_thread`

On the reference a bad access raises MemManage or BusFault, and the fault status registers
(`CFSR`, `MMFAR`, `BFAR`) say what went wrong and where. ARMv6-M has HardFault and nothing
else, and no fault status registers at all. The stacked frame is the whole report.

`HardFault_Handler` is naked (no compiler prologue) so it can read the right stack pointer
before anything moves it. It tests bit 2 of `EXC_RETURN` to pick the process or main stack,
again in Thumb-1 (`movs` plus `tst`, since `tst` takes no immediate here), and jumps to the C
reporter through a register: a Thumb-1 unconditional branch reaches only 2 KB. The reporter
first asks the kernel whether the fault can be pinned on one thread and that thread killed
(`kickos_fault_kill_thread`); on the micro:bit, which has no privilege ring, fault isolation is
not built and every fault ends the system with the dump.

**Try it:** launch the `fault` app the same way
(`build/microbit/user/apps/common/fault/fault`), `break HardFault_Handler`,
`continue`:
```
(gdb) p/x $lr
$1 = 0xfffffffd
(gdb) break kickos_armv6m_fault_report
(gdb) continue
(gdb) p/x frame[0]@8
$2 = {0x3f, 0x0, 0x3f, 0x2, 0x200009b0, 0x1329, 0xc8, 0x61000000}
```
The fault came from a thread on its process stack. The stacked `pc` (`0xc8`, inside
`kickos_app_main`) is the illegal instruction the app executes on purpose; the console then
prints `=== HARD FAULT ===` with those same registers.

**Why, and the contract:** [Book: Interrupts and traps](../book/interrupts-and-traps.md),
[Reference: porting, fault-isolation contract](../reference/porting.md)

## Where to next

- [13. ARMv8-M](13-armv8m.md): the other direction, a bigger M profile on the same switch code.
- [14. RISC-V](14-riscv.md): a core with no hardware stacking at all.
- Back to the reference: [Sleep](03-sleep.md) for the timer you saw in Stop 9,
  [Shared memory](06-shared-memory.md) for what the RP2040's MPU regions hold.

## Exercises

1. In Stop 5, the frame is eight words. Predict what `x/16wx` shows for a thread that is
   blocked in a syscall instead of a fresh one: which words are now non-zero, and where does
   its stacked `pc` point? Check it by continuing to a later hit of `*kickos_arch_mpu_commit`.
2. Stop 3 says the lock masks every interrupt. Find, in `PendSV_Handler`, the `cpsid i` that
   does not come from `IrqLock`, read the comment above it, and explain why it is needed even
   though PendSV itself only runs when `PRIMASK` is 0.
3. Stop 8 shows `control` = `0x2` after a syscall. Predict what the same command shows on the
   reference walk (`qemu`, Cortex-M4) for the same thread, and check it there.
