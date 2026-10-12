<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 13. ARMv8-M: the same walk on a Cortex-M33

The reference walk ran on a Cortex-M4 (ARMv7-M). This chapter takes it to the Cortex-M33,
an **ARMv8-M Mainline** core, on two targets: QEMU's `mps2-an505` machine (board `qemu-m33`,
chip `an505`) and the Raspberry Pi RP2350 (board `pizero2350`, chip `rp2350`). The surprise is
how little moves. For everything the arch layer touches (the `BASEPRI` critical section,
SysTick, the NVIC, PendSV, SVCall) v8-M Mainline is a superset of v7-M, so KickOS runs the
**armv7m** backend on the M33 unchanged. What differs is the memory protection unit, which
keeps its register addresses but changes what they mean. By the end you will have seen the
chip choose the new MPU backend, the base-plus-limit descriptors it writes, the security state
the image runs in, and the stack limit registers that the M33 has and KickOS leaves alone.

The concept chapters are [Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md)
and [Protection follows the CPU, not the scheduler's intent](../book/protection-follows-the-cpu-not-the-schedulers-intent.md).
The descriptor encoding is spelled out in [Reference: porting, MPU descriptor encodings](../reference/porting.md).
Read [Power-on](01-power-on.md), [The first syscall](02-first-syscall.md) and
[Shared memory](06-shared-memory.md) first: this chapter only shows what differs.

## The images we use

The `qemu-m33` build, `build/qemu-m33` (`cmake --preset qemu-m33`, then
`cmake --build --preset qemu-m33`). `ctest --test-dir build/qemu-m33 -N -V -R 'qemu_m33_hello$'`
prints the emulator line; run it halted with the gdb server on, as in [Setup](00-setup.md):

```
qemu-system-arm -M mps2-an505 -nographic -semihosting \
    -kernel build/qemu-m33/user/apps/common/hello/hello -gdb tcp::1234 -S &
gdb-multiarch build/qemu-m33/user/apps/common/hello/hello \
    -ex 'target remote :1234'
```

Stop 7 uses the `mpu_fault` image from the same build. The gdb output below is trimmed, and
the addresses are from one build. The running thread's context is `g_arch_current`, an atomic
wrapper; its raw pointer is `g_arch_current.v_._M_b._M_p`.

### Stop 1: an armv7m board with a v8-M chip
**Where:** `boards/qemu-m33/board.cmake`; `platform/an505/chip.yaml`;
`arch/arm/chip/an505/an505.ld`; `arch/arm/chip/mps2/startup.S`, `g_isr_vector`

Open the board file and the first line that matters says `set(KICKOS_ARCH "armv7m")`. A separate
`armv8m` arch directory would have duplicated the whole switch, trap and timer code to change
one file, so there is none. The chip file `platform/an505/chip.yaml` reuses the reference
chip's code (`family: { dir: arm/chip/mps2, ... }`) and states what is new in one line:

```
protection: { unit: pmsav8, granule: 32, covers_devices: true, memory_type: true }
```

The other new thing is the **security state**. An ARMv8-M core with the Security Extension
(TrustZone for M profile) can run code in a Secure and a Non-secure state, each with its own
banked stack pointers, control register and MPU. The AN505 comes out of reset Secure, and
KickOS stays there for its whole life: it uses no TrustZone partition, so "Secure" simply means
"the only state there is". The linker script places the image at the Secure aliases of the two
memory banks (code at `0x1000_0000`, SRAM at `0x3800_0000`); the RP2350 boot block marks its
image Secure for the same reason.

The vector table is the reference chip's (`KICKOS_CORE_VECTORS_V7M`). The RP2350 uses
`KICKOS_CORE_VECTORS_V8M`, which also fills slot 7, the SecureFault exception v8-M adds.

**Try it:** connect, then before `continue`:
```
(gdb) x/2wx 0x10000000
0x10000000:  0x38200000  0x10001d59
(gdb) info registers msp msp_s msp_ns control_s control_ns
msp         0x38200000
msp_s       0x38200000
msp_ns      0x0
control_s   0x0
control_ns  0x0
```
gdb shows each banked register twice. The live `msp` is the Secure one; the Non-secure bank
is never touched.

**Why, and the contract:** [Reference: porting, ARMv8-M PMSAv8](../reference/porting.md),
[Reference: boards](../reference/boards.md)

### Stop 2: arch_init pulls in the PMSAv8 backend
**Where:** `arch/arm/chip/mps2/chip_mps2.cc`, `arch_init`; `arch/arm/common/arch_arm_pmsav8.cc`,
`kickos_arm_pmsav8_init`; `cmake/chip_generate.cmake`, `kickos_chip_arm_backend`

KickOS has no weak symbols. An optional piece of the arch seam has a fallback in an archive
member of its own, and a backend replaces it by being linked first. The chip file's
`unit: pmsav8` makes `kickos_chip_arm_backend` add `arch_arm_pmsav8.cc` to the chip's archive,
but an archive member is only linked if something references it. The reference to
`kickos_arm_pmsav8_init` in `arch_init` is that anchor:

```c
void arch_init(void)
{
#if KICKOS_HAVE_MPU && KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV8
    // MUST precede kickos_armv7m_init and MUST NOT be dropped: this reference pulls
    // the PMSAv8 backend into the link. Without it the build still succeeds, but the
    // PMSAv7 commit fallback stands and writes RASR values into what is RLAR on v8-M.
    kickos_arm_pmsav8_init();
#endif
    kickos_armv7m_init();
}
```

`kickos_arm_pmsav8_init` does the one-time setup. It refuses a part with fewer descriptors than
a thread's region set needs (`MPU_TYPE.DREGION`, read from the hardware). It programs the
memory-attribute table `MPU_MAIR0`: on v8-M a region does not carry its memory type; it carries
a 3-bit index (`AttrIndx`) into eight attribute bytes, and KickOS fills three of them once:
slot 0 Normal write-back (`0xFF`), slot 1 Device (`0x04`), slot 2 Normal non-cacheable
(`0x44`). It turns on the MemManage exception, so a protection violation does not escalate to
HardFault. It leaves the MPU itself disabled: the first context switch's commit enables it.

**Try it:** `break kickos_arm_pmsav8_init`, `continue`, `finish`:
```
(gdb) x/2wx 0xE000EDC0
0xe000edc0:  0x004404ff  0x00000000
(gdb) x/wx 0xE000ED90
0xe000ed90:  0x00000800
(gdb) x/wx 0xE000ED94
0xe000ed94:  0x00000000
(gdb) x/wx 0xE000ED24
0xe000ed24:  0x00010000
```
`MAIR0` holds the three attribute bytes; `MPU_TYPE.DREGION` (bits 15:8) is 8; `MPU_CTRL` is
still 0; `SHCSR` has `MEMFAULTENA` (bit 16) set.

**Why, and the contract:** [Reference: porting, the lone-TU rule and seam fallbacks](../reference/porting.md),
[Reference: invariants, `mpu-commit-writes-what-changed`](../reference/invariants.md)

### Stop 3: the switch and the trap do not move
**Where:** `arch/arm/armv7m/switch.S`, `PendSV_Handler`, `SVC_Handler`, `svc_trampoline`;
`arch/arm/armv7m/arch_armv7m.cc`, `arch_irq_save`

Everything from [The first syscall](02-first-syscall.md) and the reference's context switch
runs here byte for byte, from the same source file. The kernel lock still raises `BASEPRI` to
`0x20`. PendSV still saves `r4`-`r11` plus `EXC_RETURN`, and, since this M33 has a
single-precision FPU, still tests bit 4 of `EXC_RETURN` and saves `s16`-`s31` only for a thread
with live floating-point state.

`EXC_RETURN` itself gains meaning on v8-M. Bit 6 (`S`) says the frame is on a Secure stack, bit
5 (`DCRS`) says the hardware used its default register stacking, bit 0 (`ES`) says the exception
was taken to the Secure state. On a v7-M core those bits read as one anyway, so for an image that
lives entirely in Secure state the values are the same numbers as on the reference:
`0xFFFFFFF9` for the first PendSV, which interrupts boot code on the main stack, and
`0xFFFFFFFD` for every later one. The switch code compares and stores the whole word and never
has to know.

**Try it:** `break arch_start`, `continue`, then `break arch_switch` and `continue`:
```
(gdb) info registers basepri primask control
basepri  0x20
primask  0x0
control  0x2
```
Then `delete`, `break PendSV_Handler` and `continue` a few times with `p/x $lr`: the first
reads `0xfffffff9`, the following ones `0xfffffffd`.

**Why, and the contract:** [Book: Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md),
[Reference: porting, context switch and first-thread start](../reference/porting.md)

### Stop 4: the commit, with the MPU off
**Where:** `arch/arm/common/arch_arm_pmsav8.cc`, `kickos_arch_mpu_commit`

PendSV calls `kickos_arch_mpu_commit` after the physical swap, exactly as on the reference.
On the M33 the symbol resolves to the PMSAv8 version, which replaces the PMSAv7 fallback. Two
things differ from the v7 writer you saw in [Shared memory](06-shared-memory.md).

It brackets the whole reprogram with `MPU_CTRL = 0`: this revision gives no way to move one
descriptor while the MPU is live, so the commit turns the unit off, rewrites, and turns it back
on with `ENABLE | PRIVDEFENA` (the second bit gives privileged code the default memory map, so
the kernel needs no region of its own). The v7 writer instead disables one descriptor at a
time and never turns the unit off. The commit masks interrupts across the window with
`cpsid i`, because PendSV runs at the lowest priority and the kernel's `BASEPRI` lock is no
longer held by then.

It still writes only what changed. A per-core record `g_mpu_held` remembers the words last
written, and the loop skips a slot whose two words already stand:

```c
        if (not total and held->rbar[i] == img->rbar[i] and held->rlar[i] == img->rlar[i])
        {
            continue;
        }
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        reg32(MPU_RBAR) = img->rbar[i];
        reg32(MPU_RLAR) = img->rlar[i];
        held->rbar[i] = img->rbar[i];
        held->rlar[i] = img->rlar[i];
```

`MPU_RNR` selects the descriptor; `MPU_RBAR` and `MPU_RLAR` are then that descriptor's two words.

**Try it:** `break arch_start`, `continue`, `delete`, `break kickos_arch_mpu_commit`,
`continue`, `finish`. You are back in `PendSV_Handler`, after the first commit:
```
(gdb) x/wx 0xE000ED94
0xe000ed94:  0x00000005
(gdb) set {unsigned}0xE000ED98 = 0
(gdb) x/2wx 0xE000ED9C
0xe000ed9c:  0x10000006  0x103fffe1
(gdb) set {unsigned}0xE000ED98 = 1
(gdb) x/2wx 0xE000ED9C
0xe000ed9c:  0x38010003  0x3801ffe1
(gdb) set {unsigned}0xE000ED98 = 2
(gdb) x/2wx 0xE000ED9C
0xe000ed9c:  0x38026003  0x38027fe1
(gdb) set {unsigned}0xE000ED98 = 3
(gdb) x/2wx 0xE000ED9C
0xe000ed9c:  0x00000000  0x00000000
(gdb) p *g_arch_current.v_._M_b._M_p
$1 = {sp = 939687868, npriv = 1, resting_npriv = 1, stack_lo = 939679744, stack_hi = 939687936, kernel_sp = 939526208}
```
`MPU_CTRL` = 5 (`ENABLE` and `PRIVDEFENA`). Writing `MPU_RNR` from gdb is safe here: the
commit sets it before every write. Stop 5 decodes the three descriptors.

**Why, and the contract:** [Reference: invariants, `mpu-apply-on-every-switch-in` and `mpu-bypass-windows-accepted`](../reference/invariants.md),
[Book: Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md)

### Stop 5: reading a base-plus-limit descriptor
**Where:** `arch/arm/common/pmsav8_encode.h`, `pmsav8_encode`, `pmsav8_rbar_attr`,
`pmsav8_rlar_attr`; `arch/arm/common/regs_v8m.h`; `arch/arm/common/arch_arm_pmsav8.cc`,
`arch_mpu_region_encodable`

On v7-M a region is a base and a power-of-two size, and it must be aligned to its own size.
PMSAv8 states the region as a base and an inclusive limit, each on a 32-byte boundary, so any
multiple of 32 bytes at any 32-byte-aligned address can be named exactly. That is the whole of
`arch_mpu_region_encodable` on this backend, and why a stack or a buffer needs no rounding up
here.

The access rights moved too. On v7-M they sit in `RASR` beside the size. On v8-M they sit in the
low bits of `RBAR`, next to the base: `XN` (execute-never) in bit 0 and `AP` (access permission)
in bits 2:1. `RLAR` carries the limit, the `AttrIndx` into `MAIR`, and the enable bit:

```c
        inline uint32_t pmsav8_rbar_attr(uint32_t attr)
        {
            if (attr & ARCH_MPU_X)
            {
                return RBAR_AP_RO_ANY; // code: RO-any, executable (XN=0), SH=0
            }
            uint32_t v = RBAR_XN; // data / MMIO: execute-never
            if (attr & ARCH_MPU_W)
            {
                v |= RBAR_AP_RW_ANY;
            }
            else
            {
                v |= RBAR_AP_RO_ANY;
            }
            return v;
        }
```

Now decode Stop 4's words. Descriptor 0: base `0x10000000`, `AP` = `11` (read-only, any
privilege), `XN` clear; limit `0x103fffe0` plus 31, so up to `0x103fffff`; `AttrIndx` 0
(Normal); enabled. That is the 4 MiB code bank, readable and executable by the thread.
Descriptor 1: `0x38010000` to `0x3801ffff`, `AP` = `01` (read-write, any), `XN` set: the
application's data. Descriptor 2: `0x38026000` to `0x38027fff`, read-write, execute-never: the
thread's own 8 KiB stack, and you can check it against `stack_lo` and `stack_hi` (`0x38026000`
and `0x38028000`) in the context printed above. Descriptor 3 is disabled.

A region the encoder cannot name exactly gets `RLAR` = 0, which leaves the slot disabled. It is
never widened to the nearest boundary, because widening would grant bytes nobody asked for.

One rule changed with no new register to show it: overlap. On v7-M, where two enabled regions
cover the same byte, the higher-numbered one decides. On v8-M the access faults, a privileged
one included, and the `PRIVDEFENA` background does not rescue it. Each backend names its rule
as `ARCH_MPU_OVERLAP` in its `mpu_encoded.h` (`ARCH_MPU_OVERLAP_FAULTS` here,
`ARCH_MPU_OVERLAP_HIGHER` for PMSAv7). `MpuSet::admits` (`kernel/include/kickos/mpuset.h`)
reads it when a grant is added: under the faulting rule, a new region that would overlap one
already seated is refused rather than programmed.

**Try it:** the decode above is of Stop 4's output, so there is no separate stop. To see the
encoder and the overlap rule run on the host, read `tests/unit/mpuoverlap/hw_armv7m_pmsav8.cc`,
which feeds `pmsav8_encode` a region set and asks what the hardware would decide.
<!-- unverified: no gdb stop of its own; the decode is of Stop 4's verified output -->

**Why, and the contract:** [Reference: porting, ARMv8-M PMSAv8](../reference/porting.md)

### Stop 6: stack limit registers, present and unused
**Where:** `arch/arm/armv7m/switch.S`, `PSP_GUARD`; `arch/arm/armv7m/include/kickos/arch/armv7m_trap_stack.h`

ARMv8-M Mainline adds two registers v7-M lacks: `MSPLIM` and `PSPLIM`, one per stack pointer.
When set, the core raises a UsageFault (`STKOF`) the moment a push would take the stack
pointer below the limit. It is a hardware stack-overflow check.

KickOS does not program them. Because the M33 runs the armv7m backend unchanged, it guards its
stacks the way the reference does: in software, `PSP_GUARD` in `PendSV_Handler` and
`SVC_Handler` refuses a process stack pointer with no room below it for the frame about to be
pushed, and under enforcement the thread's MPU stack region (descriptor 2 above) faults any
access past either end. The limit registers stay at their reset value, 0, which means no
check.

**Try it:** run to the end of the first commit as in Stop 4, then:
```
(gdb) info registers psp psplim msplim control
psp     0x38027fe0
psplim  0x0
msplim  0x0
control 0x1
```
A thread is about to resume on its process stack, and no limit is set. (`control` shows
`nPRIV` = 1 for the unprivileged thread; in handler mode the stack-select bit reads 0.)

**Why, and the contract:** [Book: Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md),
[Book: Thread stacks](../book/thread-stacks-and-the-kiss-tension.md)

### Stop 7: a violation is still a MemManage
**Where:** `arch/arm/armv7m/arch_armv7m.cc`, `HardFault_Handler`, `kickos_armv7m_fault_report`;
`arch/arm/common/core_vectors.inc`, `KICKOS_CORE_VECTORS_V7M`

PMSAv8 reports a violation exactly as v7-M does: a MemManage exception, with `CFSR` and
`MMFAR` saying what and where. So the reference's fault path needs no change. The vector block
points the MemManage slot at the same `HardFault_Handler`, which reads the fault registers and
hands the frame to the kernel; with fault isolation on, the kernel kills the faulting thread and
the system continues.

**Try it:** launch `build/qemu-m33/user/apps/common/mpu_fault/mpu_fault`
stopped, `break HardFault_Handler`, `continue`:
```
(gdb) p/x $xpsr & 0x1ff
$1 = 0x4
(gdb) x/wx 0xE000ED28
0xe000ed28:  0x00000082
(gdb) x/wx 0xE000ED34
0xe000ed34:  0x38029000
(gdb) p/x $lr
$2 = 0xfffffffd
```
The exception number in `xPSR` is 4, MemManage. `CFSR` = `0x82`: `DACCVIOL` (a data access
violation) and `MMARVALID` (the address register holds the faulting address). `MMFAR` is the
address the thread touched, one no region of its set covers. After `continue` the console prints
`=== THREAD FAULT === thread 'domainA' killed, system continues`.

**Why, and the contract:** [Book: Interrupts and traps](../book/interrupts-and-traps.md),
[Reference: porting, fault-isolation contract](../reference/porting.md)

## Where to next

- [12. ARMv6-M](12-armv6m.md): the other direction, the smallest M profile.
- [14. RISC-V](14-riscv.md): a protection unit that fails closed.
- Back to the reference: [Shared memory](06-shared-memory.md), the v7 writer this backend replaces.

## Exercises

1. Stop 5 says PMSAv8 needs no power-of-two rounding. Take descriptor 1 (`0x38010000`, 64 KiB)
   and work out whether PMSAv7 could describe it in one region. Then pick a 48 KiB buffer at
   `0x38010000` and answer the same question for both revisions.
2. In Stop 4, `finish` once more from a later commit, between two threads of the same task.
   Predict which of the three descriptors the commit actually rewrites, then step through the
   loop to check.
3. Stop 6: if `PSPLIM` were set to each thread's `stack_lo` at switch-in, which software check
   would it duplicate, and which kind of overflow would it catch earlier? Find in
   `PendSV_Handler` where the write would have to go so that a thread never runs with another
   thread's limit.
