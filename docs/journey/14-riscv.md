<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 14. RISC-V: the same walk on RV32, and a look at RV64

The reference walk ran on a Cortex-M4. This chapter takes the same stops to RISC-V, and here
more changes than on any ARM variant, because RISC-V gives the kernel far less for free. There
is no hardware exception frame: a trap saves nothing, so the software saves every register.
There is no separate "handler stack": a trap arrives on whatever stack pointer was live. There
is no PendSV: the deferred switch is a software interrupt the kernel raises against itself.
And the protection unit, PMP, fails closed where the ARM MPU fails open.

The main walk is the 32-bit port, **RV32IMAC in machine mode with PMP**, on QEMU's `virt`
machine (board `qemu-riscv`, chip `virt_rv32`); its silicon twin is the ESP32-C6 (board
`esp32c6-wroom`, chip `esp32c6`). The last stop looks briefly at the 64-bit port (board
`qemu-riscv64-smp`, chip `virt_rv64`), which runs in supervisor mode with page tables and is a
different port, not a wider copy of this one.

The concept chapter is [Porting a new ISA: RISC-V](../book/porting-a-new-isa-riscv.md); it
explains the single-frame deferred switch and the trampoline, and this chapter shows them
running. The arch seam is [Reference: porting](../reference/porting.md), whose RV32IMAC
section is the exact contract. Read [Power-on](01-power-on.md) and
[The first syscall](02-first-syscall.md) first: this chapter shows what differs.

## The images we use

The `qemu-riscv` build, `build/qemu-riscv` (`cmake --preset qemu-riscv`, then
`cmake --build --preset qemu-riscv`). `ctest --test-dir build/qemu-riscv -N -V -R 'hello$'`
prints the emulator line; run it halted with the gdb server on, as in [Setup](00-setup.md):

```
qemu-system-riscv32 -M virt -bios none -nographic -semihosting \
    -kernel build/qemu-riscv/user/apps/common/hello/hello -gdb tcp::1234 -S &
gdb-multiarch build/qemu-riscv/user/apps/common/hello/hello \
    -ex 'target remote :1234'
```

`-bios none` means no firmware: the image itself runs in machine mode from the first
instruction. QEMU's gdb stub shows the control and status registers (CSRs) by name
(`mstatus`, `mcause`, `pmpcfg0` ...) and adds a virtual register `$priv`, the current privilege
level: 3 is machine mode (M), 1 supervisor (S), 0 user (U). The gdb output below is trimmed, and
addresses are from one build. The running thread's context is `g_arch_current`, an atomic
wrapper; its raw pointer is `g_arch_current.v_._M_b._M_p`.

### Stop 1: reset, with no vector table to read
**Where:** `arch/riscv/chip/virt_rv32/startup.S`, `_start`; `arch/riscv/chip/virt_rv32/chip_virt_rv32.cc`,
`Reset_Handler`

A Cortex-M core reads its first stack pointer and entry address out of a table in memory. A
RISC-V core just starts executing at an address fixed by the machine. On QEMU `virt` that is a
few instructions of boot ROM at `0x1000`, which jump to the image at `0x8000_0000`, in machine
mode. Nothing has set a stack pointer, so `_start` is a few lines of assembly that set the
global pointer, the stack pointer and the thread pointer and then call C:

```
_start:
    /* gp := __global_pointer$ with relaxation OFF (a relaxed la would itself be
       gp-relative, but gp is not set yet). Standard RISC-V boot idiom. */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, _estack             /* boot/handler stack: top of RAM */
    li      tp, 0                   /* no TLS: thread pointer parked at 0 */
    call    Reset_Handler           /* C runtime + kmain (noreturn) */
```

The **global pointer** `gp` is a RISC-V convention the reference has no twin for: the linker
may address small globals as `gp` plus a 12-bit offset, so `gp` must hold the agreed anchor
`__global_pointer$` before any C runs. `Reset_Handler` then does what the reference's does:
`kickos_ranges_init`, then `kickos_crt_tail` into `arch_init` and `kmain`. One more difference:
no trap handler is installed yet (`mtvec` is still unset), so nothing may trap until `arch_init`
has run.

**Try it:** `break _start`, `continue`:
```
(gdb) info registers pc sp
pc   0x80000000   <_start>
sp   0x0
(gdb) p $priv
$1 = 3
```

**Why, and the contract:** [Book: Context switching, startup](../book/context-switching-and-the-silicon-contract.md),
[Reference: porting, the QEMU verification target](../reference/porting.md)

### Stop 2: arch_init sets up the trap, the timer and a permissive PMP
**Where:** `arch/riscv/chip/virt_rv32/chip_virt_rv32.cc`, `arch_init`;
`arch/riscv/rv32imac/arch_rv32imac.cc`, `kickos_rv32_init`; `arch/riscv/rv32imac/switch.S`,
`kickos_rv_mtvec`

The chip half of `arch_init` points `g_clint_msip` at the CLINT's software-interrupt register
(Stop 4) and parks the timer comparator `mtimecmp` at its maximum, so no timer interrupt fires
before one is asked for. Then `kickos_rv32_init` sets up the core, one CSR at a time:

- `mtvec`, the trap vector, in **vectored** mode: an interrupt of cause *i* jumps to base +
  4*i. KickOS fills every slot of the table `kickos_rv_mtvec` with the same `j trap_entry`, so in
  effect there is one door. It uses vectored mode only because the ESP32-C6 supports nothing
  else.
- `mscratch`, a scratch CSR only machine mode can touch, gets the top of a small per-hart
  **trap stack** `g_rv_trap_stack`. Stop 5 shows why.
- `mie` enables three interrupt sources: machine software (the switch), machine timer, and
  supervisor software (a test channel for injected interrupts).
- `mcounteren` lets user code read the cycle and time counters.
- The **bootstrap PMP entry**. On ARM, an unprivileged thread with no MPU region can reach
  everything. RISC-V PMP is the other way round: once a core implements PMP, a user-mode access
  that matches no entry faults. Without this entry the first user thread could not fetch its
  first instruction.

```c
#if !KICKOS_RV32_LP
    __asm volatile("csrw pmpaddr0, %0" ::"r"(0xFFFFFFFFu) : "memory");
    __asm volatile("csrw pmpcfg0, %0" ::"r"(0x0Fu) : "memory");
#endif
```

`pmpcfg0` = `0x0F` is entry 0's configuration byte: mode TOR ("top of range", the entry covers
everything below `pmpaddr0 << 2`) plus read, write and execute. The first context switch
replaces it with the thread's real regions (Stop 10).

**Try it:** `break kickos_rv32_init`, `continue`, `finish`:
```
(gdb) info registers mtvec mscratch mie pmpcfg0 pmpaddr0
mtvec     0x80002701
mscratch  0x80048870
mie       0x8a
pmpcfg0   0xf
pmpaddr0  0xffffffff
(gdb) p/x &kickos_rv_mtvec
$2 = 0x80002700
(gdb) p/x &g_rv_trap_stack
$3 = 0x800484b0
```
`mtvec` is the table address with mode bit 0 set (vectored). `mscratch` is the end of the trap
stack. `mie` = `0x8a`: bits 1, 3 and 7.

**Why, and the contract:** [Book: Porting a new ISA, three RISC-V specifics](../book/porting-a-new-isa-riscv.md),
[Reference: porting, the RISC-V RV32IMAC arch](../reference/porting.md)

### Stop 3: the critical section is mstatus.MIE
**Where:** `arch/riscv/rv32imac/include/kickos/arch/irq_inline.h`, `arch_irq_save`, `arch_irq_restore`

Machine mode has one global interrupt enable, the `MIE` bit (bit 3) of `mstatus`. The lock
clears it and returns the old value of that one bit; the unlock sets it back only if it was set:

```c
static inline __attribute__((always_inline)) arch_irq_state_t arch_irq_save(void)
{
    uint32_t old;
    __asm volatile("csrrci %0, mstatus, 0x8" : "=r"(old)::"memory");
    return old & KICKOS_RV32_MSTATUS_MIE;
}

static inline __attribute__((always_inline)) void arch_irq_restore(arch_irq_state_t state)
{
    // csrs only SETS bits, and state is 0 or MSTATUS_MIE, so this re-enables MIE exactly
    // when the paired save disabled it. That is what makes it nesting-safe.
    __asm volatile("csrs mstatus, %0" ::"r"(state) : "memory");
}
```

`csrrci` reads the CSR and clears the given bits in one instruction, so there is no window
between the read and the mask. Like ARMv6-M's `PRIMASK` it masks everything; there is no
priority band.

**Try it:** `break arch_switch`, `continue`:
```
(gdb) p/x $mstatus & 8
$2 = 0x0
```

**Why, and the contract:** [Reference: porting, the critical section](../reference/porting.md)

### Stop 4: arch_switch rings the kernel's own doorbell
**Where:** `arch/riscv/rv32imac/arch_rv32imac.cc`, `arch_switch`;
`arch/riscv/chip/virt_rv32/chip_virt_rv32.cc`, `arch_init`

On the reference, `arch_switch` sets PendSV pending. RISC-V has no PendSV, so KickOS uses the
**machine software interrupt**: the CLINT (the core-local interruptor, which also holds the
timer) has one memory-mapped `msip` word per hart, and writing 1 to it raises an interrupt on
that hart. With `MIE` clear the interrupt stays pending until the lock is released; then it is
taken at once, and the switch runs in its trap:

```c
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    (void)from; // the switcher saves g_arch_current
    g_arch_next = to;
#if KICKOS_RV32_LP
    // ...
#else
    *g_clint_msip = 1; // pend machine software interrupt
#endif
}
```

**Try it:** continue from Stop 3, then `finish` out of `arch_switch`:
```
(gdb) x/wx 0x2000000
0x2000000:  0x00000001
```
`0x0200_0000` is the CLINT base on `virt`, and hart 0's `msip` is its first word.

**Why, and the contract:** [Book: Porting a new ISA, the single-frame deferred switch](../book/porting-a-new-isa-riscv.md),
[Reference: invariants, `deferred-switch-lowest-band`](../reference/invariants.md)

### Stop 5: one door, and a stack it can trust
**Where:** `arch/riscv/rv32imac/switch.S`, `trap_entry`

Every trap (an interrupt, an `ecall`, a fault) enters `trap_entry`. On a Cortex-M the hardware
would now have pushed half the registers onto a stack it chose. Here nothing has been saved,
and `sp` is whatever the interrupted code had. If that code was a user thread, `sp` is a value
the thread chose, and machine mode ignores the PMP entries the kernel programs, so storing
through it could write anywhere. So the first instruction swaps `sp` with `mscratch`, which
holds the trusted trap-stack top:

```
trap_entry:
    /* Swap onto the trusted trap stack before touching the interrupted sp. A U-mode thread
       can aim its sp at kernel memory, and the stores below run in M-mode, past the unlocked
       PMP entries. After the swap mscratch holds the interrupted sp. */
    csrrw   sp, mscratch, sp
    sw      t0, -4(sp)
    sw      t1, -8(sp)
```

Then it re-seats `gp` (user code can change it too), reads the previous privilege from the `MPP`
field of `mstatus` (bits 12:11), and picks where the 128-byte frame goes. There are three
places:

- **From user mode**: the interrupted `sp` must be aligned and inside the thread's own stack
  (`stack_lo` to `stack_hi`); a stack pointer that fails is refused and the thread is contained.
  Then the frame goes on the thread's own **kernel stack** (`ctx.kernel_sp`), never on the user
  stack. This arch selects `ARCH_KERNEL_STACKS_MANDATORY`, so every thread has one.
- **From machine mode, for the switch interrupt or an `ecall`**: the frame stays on the
  interrupted stack, because it becomes that thread's saved context.
- **Any other machine-mode trap**: the frame goes on the trap stack and is popped on return.

Then it stores 28 general registers, `mepc` (the interrupted program counter) and `mstatus`, and
reads `mcause` to learn why it is here. `gp` and `tp` are not saved: the kernel rewrites both
from its own values on every return.

**Try it:** `break arch_start`, `continue`, then catch a user `ecall` at the door with
`break trap_entry if $mcause == 8`, `continue`:
```
(gdb) p/x $mstatus
$1 = 0x80
(gdb) info registers sp mscratch
sp        0x8005bf80
mscratch  0x80048870
(gdb) p *g_arch_current.v_._M_b._M_p
$2 = {sp = 2147860352, stack_lo = 2147852288, stack_hi = 2147860480, kernel_sp = 2147747056, tls_base = 2147852288}
(gdb) stepi 3
(gdb) info registers sp mscratch
sp        0x80048870
mscratch  0x8005bf80
```
`MPP` is 0, so the trap came from user mode. Before the swap `sp` is the user's (inside
`0x8005a000`-`0x8005c000`); after it, `sp` is the trap-stack top and `mscratch` holds the
user's `sp` for the bounds test.

**Why, and the contract:** [Book: Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md),
[Book: Interrupts and traps](../book/interrupts-and-traps.md)

### Stop 6: the switcher, and a thread frozen in the kernel
**Where:** `arch/riscv/rv32imac/switch.S`, `trap_entry` (its `.Lswitch` and `.Lrestore` parts),
`arch_start`; `arch/riscv/rv32imac/arch_rv32imac.cc`, `arch_context_init`

When `mcause` says machine software interrupt (`0x80000003`), the code clears `msip`, stores the
`sp` (which now points at the frame it just saved) into the outgoing context, loads the
incoming context's `sp`, calls `kickos_arch_mpu_commit` (Stop 10) and falls into the shared
restore path, which reloads every register from the incoming frame and executes `mret`. `mret`
is the return instruction: it jumps to `mepc` and sets the privilege to `mstatus.MPP`, both
taken from the frame.

That is the whole switch, and it is the same for every reason to switch. A thread that blocked
in a syscall was running kernel code in machine mode when the msip interrupt froze it, so its
frame records machine mode and a `mepc` inside the kernel; it resumes there, mid-function. A
thread preempted in user code resumes in user mode. There is one frame format and one restore.

The first thread starts differently from the reference. ARM's `arch_start` pends PendSV and
lets the first switch happen as an exception. Here `arch_start` loads the first context's `sp`
and jumps straight into the restore path, and the frame `arch_context_init` fabricated does the
rest: `mepc` = the entry, `mstatus` with `MPIE` set (so `mret` turns interrupts on) and `MPP` =
user for an unprivileged thread, `ra` = the return stub, `a0` = the argument.

**Try it:** first watch a thread being frozen. `break arch_start`, `continue`,
`break trap_entry if $mcause == 0x80000003`, `continue`:
```
(gdb) p/x $mstatus
$2 = 0x1880
(gdb) info registers sp
sp   0x80040400   <kickos::(anonymous namespace)::g_kstacks+944>
(gdb) info symbol 0x8000de68
kickos::notify_wait(...) + 196 in section .text
```
(`0x8000de68` is `mepc`; `p/a $mepc` may print it without a name.) `MPP` = 3: the interrupted
code was machine mode, on a kernel stack, inside `notify_wait`. Then watch a thread being
restored: `delete`, `break *kickos_arch_mpu_commit if $mcause == 0x80000003`, `continue`:
```
(gdb) info registers sp
sp   0x8005ff80
(gdb) x/2wx $sp
0x8005ff80:  0x80001c5e  0x00000080
(gdb) info symbol *(unsigned*)$sp
kickos::init::task_trampoline(void*) in section .text
```
`sp` is the incoming frame. Its first two words are `mepc` and `mstatus`: a fresh thread about
to enter `task_trampoline` in user mode (`MPP` = 0, `MPIE` = 1).

**Why, and the contract:** [Book: Porting a new ISA, the single-frame deferred switch](../book/porting-a-new-isa-riscv.md),
[Reference: invariants, `switch-frame-matches-init` and `trap-return-atomic`](../reference/invariants.md)

### Stop 7: into user mode
**Where:** `arch/riscv/rv32imac/switch.S`, `.Lrestore` (inside `trap_entry`); `user/src/root_entry.cc`,
`kickos_root_entry`

The restore path writes `gp` from `__global_pointer$` and `tp` from the incoming context's
`tls_base` before anything else, so a thread can never hand its own values of either to the
next. It writes `mstatus` before `mepc` (the reason is in the `trap-return-atomic` invariant),
reloads the registers, loads `sp` last, and `mret`s.

**Try it:** `break arch_start`, `continue`, `tbreak kickos_root_entry`, `continue`:
```
(gdb) p $priv
$1 = 0
(gdb) info registers sp gp tp
sp   0x8005c000
gp   0x80050d78
tp   0x8005a000
```
The first thread runs in user mode. `tp` is the context's `tls_base`, which
`arch_context_init` sets to `stack_lo` minus the size of the thread-local block; in this image
it reads the same as `stack_lo`.

**Why, and the contract:** [Book: Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md)

### Stop 8: ecall and the trampoline
**Where:** `arch/riscv/rv32imac/switch.S`, `arch_syscall`, `trap_entry` (its `.Lecall` part),
`svc_trampoline`

The user stub is two instructions, `ecall` then `ret`: the arguments are already in `a0`-`a4`,
so nothing has to be shuffled. `trap_entry` saves the frame on the caller's kernel stack and
sees `mcause` = 8 (`ecall` from user mode). Syscall 56 tries the register-IPC fast path first,
as on ARM. Otherwise the trap does what the ARM SVC handler does with a stacked `pc`: it rewrites
`mepc` to `svc_trampoline`, sets `MPP` to machine and `MPIE` to 1, and `mret`s. The trampoline
therefore runs as ordinary privileged code, with interrupts on, on the caller's kernel stack,
and a blocking syscall is frozen there by the msip switch of Stop 6:

```
svc_trampoline:
    call    syscall_dispatch        /* (a0..a4) -> result in a0:a1 */
    /* BOTH halves: the restore epilogue reloads a0-a4 from the frame, so a result half left
       only in a register is discarded by mret. */
    sw      a0, F_A0(sp)            /* frame.a0 = result low */
    sw      a1, F_A1(sp)            /* frame.a1 = result high */
    lw      t0, F_MEPC(sp)          /* ecall pc */
    addi    t0, t0, 4
    sw      t0, F_MEPC(sp)
    j       .Lrestore
```

The return goes through the same restore path as a switch. The frame still holds the caller's
`mstatus`, so `mret` drops back to whatever privilege the caller had, and `mepc` + 4 skips the
`ecall` (which is always 4 bytes long). No "resting privilege" field is needed, unlike ARM's
`resting_npriv`: the saved `MPP` is the caller's privilege.

**Try it:** `break arch_start`, `continue`, `break svc_trampoline`, `continue`:
```
(gdb) p $priv
$1 = 3
(gdb) p/x $mcause
$2 = 0x8
(gdb) info registers a0 sp
a0   0x47
sp   0x80040470   <kickos::(anonymous namespace)::g_kstacks+1056>
(gdb) x/2wx $sp
0x80040470:  0x80002b3a  0x00000080
(gdb) info symbol *(unsigned*)$sp
arch_syscall64 in section .text
```
Machine mode, on a kernel stack, `a0` = 71 (the same `kos_thread_self` as on ARM). The frame's
`mepc` is the user stub's `ecall` and its `mstatus` says the caller was in user mode.

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md),
[Book: Porting a new ISA, the system call](../book/porting-a-new-isa-riscv.md)

### Stop 9: the timer is the CLINT comparator
**Where:** `arch/riscv/chip/virt_rv32/chip_virt_rv32.cc`, `arch_clock_now`, `arch_timer_arm`;
`arch/riscv/rv32imac/switch.S`, `trap_entry` (its `.Ltimer` part)

The clock and the one-shot timer are both CLINT registers here: `mtime`, a 64-bit counter at
10 MHz, and `mtimecmp`, a 64-bit comparator. The timer interrupt is pending while
`mtime >= mtimecmp`. On a 32-bit core the comparator is written in two halves, and a careless
order could match a half-written value, so `arch_timer_arm` parks the high half at all-ones
first:

```c
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t ticks = deadline_ns / NS_PER_TICK;
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
    // Safe 64-bit write on RV32: park the high half so no spurious match can fire
    // between the two 32-bit stores, then commit lo then hi.
    cmp[1] = 0xFFFFFFFFu;
    cmp[0] = static_cast<uint32_t>(ticks);
    cmp[1] = static_cast<uint32_t>(ticks >> 32);
}
```

The trap path for cause `0x80000007` raises `g_isr_depth` (what `arch_in_isr` reads), calls the
kernel's `kickos_isr_timer`, and lowers it again. If the timer woke a thread, the switch it asks
for pends msip, which is taken right after the `mret`.

**Try it:** `break kickos_isr_timer`, `continue`:
```
(gdb) p/x $mcause
$1 = 0x80000007
(gdb) x/2wx 0x2004000
0x2004000:  0x003e7759  0x00000000
(gdb) x/2wx 0x200bff8
0x200bff8:  0x003e9aed  0x00000000
```
`mtimecmp` (at CLINT + `0x4000`) has been passed by `mtime` (at CLINT + `0xBFF8`).

**Why, and the contract:** [Book: The time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md),
[Sleep](03-sleep.md)

### Stop 10: PMP, programmed whole on every switch
**Where:** `arch/riscv/rv32imac/arch_rv32imac.cc`, `kickos_arch_mpu_commit`,
`arch_mpu_region_encodable`; `arch/riscv/rv32imac/pmp_encode.h`, `pmp_encode`,
`pmp_napot_addr`; `arch/riscv/rv32imac/include/kickos/arch/mpu_encoded.h`

PMP has numbered entries, each an address CSR (`pmpaddr0`-`pmpaddr7` here) plus one
configuration byte, packed four to a `pmpcfg` CSR. KickOS uses **NAPOT** entries ("naturally
aligned power of two"): the size is encoded in the address itself, as a run of trailing one
bits, so a region must be a power of two of at least 8 bytes, aligned to its size. Where two
entries match, the lowest-numbered one decides. Machine mode is not checked against these
entries at all, which is why the kernel needs no region of its own (and why Stop 5 cannot trust
a user `sp`).

The commit runs from the switcher with `MIE` clear and writes all eight addresses and both
configuration words, every time. Skipping the entries that did not change was measured and
found to cost more than writing them, because CSR numbers are immediates and each skip needs a
load and a branch.

**Try it:** `break arch_start`, `continue`, `break kickos_arch_mpu_commit if $mcause == 0x80000003`,
`continue`, `finish`:
```
(gdb) p *g_arch_current.v_._M_b._M_p
$1 = {sp = 2147876736, stack_lo = 2147868672, stack_hi = 2147876864, kernel_sp = 2147748240, tls_base = 2147868672}
(gdb) info registers pmpcfg0 pmpcfg1 pmpaddr0 pmpaddr1 pmpaddr2 pmpaddr3
pmpcfg0   0x1b1b1d
pmpcfg1   0x0
pmpaddr0  0x20007fff
pmpaddr1  0x20014fff
pmpaddr2  0x20017bff
pmpaddr3  0x0
```
Decode the configuration bytes from the low end: entry 0 is `0x1d` (NAPOT, execute, read),
entries 1 and 2 are `0x1b` (NAPOT, write, read), and entry 3 is 0, off. Decode an address by
counting its trailing ones *n*: the size is 2^(n+3) bytes, and the base is the address with
those bits cleared, shifted left by 2. Entry 0, 15 ones: 256 KiB at `0x8000_0000`, the code.
Entry 1, 12 ones: 32 KiB at `0x8005_0000`, the application's data. Entry 2, 10 ones: 8 KiB at
`0x8005_e000`, which is `stack_lo` in the context above: the thread's own stack. The bootstrap
entry of Stop 2 is gone.

**Why, and the contract:** [Book: Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Reference: invariants, `mpu-apply-on-every-switch-in` and `mpu-commit-writes-what-changed`](../reference/invariants.md)

### Stop 11: a PMP fault
**Where:** `arch/riscv/rv32imac/switch.S`, `trap_entry` (its `.Lfault` part);
`arch/riscv/rv32imac/arch_rv32imac.cc`, `kickos_rv_fault_report`

A user access that no entry allows raises an access-fault exception: `mcause` 1 for a fetch, 5
for a load, 7 for a store, with the address in `mtval`. RISC-V needs no status register like
ARM's `CFSR`: the cause code already says which kind. `.Lfault` passes the cause, `mepc`,
`mtval` and `mstatus` to `kickos_rv_fault_report`, which first asks the kernel to kill just the
faulting thread. When that succeeds it rewrites `mepc` to the thread's exit stub and returns
true, and `.Lfault` `mret`s there instead of printing a dump.

**Try it:** launch `build/qemu-riscv/user/apps/common/mpu_fault/mpu_fault`
stopped, `break kickos_rv_fault_report`, `continue`:
```
Breakpoint 1, kickos_rv_fault_report (mcause=7, mepc=2147483736, mtval=2147880960, mstatus=128, frame=0x80040db0 <...g_kstacks+3424>)
```
A store access fault (7) from user mode (`mstatus` = `0x80`, `MPP` = 0) at `mtval` =
`0x80061000`. After `continue` the console prints `=== THREAD FAULT === thread 'domainA' killed,
system continues`.

**Why, and the contract:** [Book: Interrupts and traps](../book/interrupts-and-traps.md),
[Reference: porting, fault-isolation contract](../reference/porting.md)

### Stop 12: the ESP32-C6, where silicon disagrees with QEMU
**Where:** `arch/riscv/chip/esp32c6/chip_esp32c6.cc`, `arch_rv_has_mcounteren`,
`arch_rv_inject_deliver`, `arch_clock_now`; `platform/esp32c6/chip.yaml`; `arch/Kconfig`,
`CHIP_ESP32C6`

The same `arch/riscv/rv32imac/` code runs on the ESP32-C6, and every place it bends is a fact
about that core:

- Its trap vector supports vectored mode only, which is why the shared code uses vectored mode
  everywhere (Stop 2).
- It traps on any write to `mcounteren`, so the chip answers `arch_rv_has_mcounteren` with 0
  and the shared bring-up skips the write. It has no cycle counter instruction either
  (`rdcycle` is illegal in every mode), so the chip selects no trace clock.
- Its PMP does not treat an all-ones NAPOT entry as "match everything", which is why the
  bootstrap entry of Stop 2 is TOR, which works on both.
- It has machine and user mode only, no supervisor mode, so the supervisor software interrupt
  that injects test interrupts on QEMU does nothing there; `arch_rv_inject_deliver` raises a "from CPU" interrupt line of the chip's
  interrupt controller instead.
- Behind the PMP sits a second gate, the APM, which checks bus accesses per security mode, not
  per thread. The chip file describes it (`bus_gate: { kind: apm, ... }`), and the chip opens it
  once at boot for user mode. A denied access there does not trap: a read returns 0 and a write
  is dropped.

**Try it:** there is no ESP32-C6 model in QEMU, so this stop is read from the code.
<!-- unverified: no emulator for the ESP32-C6 -->

**Why, and the contract:** [Reference: porting, the RISC-V RV32IMAC arch](../reference/porting.md),
[Book: Peripheral isolation and the hardware ceiling](../book/peripheral-isolation-and-the-hardware-ceiling.md)

### Stop 13: RV64, supervisor mode and page tables
**Where:** `arch/riscv/chip/virt_rv64/startup.S`, `_start`; `arch/riscv/rv64imac/switch.S`,
`kickos_rv64_stvec`, `kickos_rv64_switch_now`; `arch/riscv/rv64imac/arch_rv64imac.cc`,
`arch_switch`; `arch/riscv/rv64imac/aspace_rv64imac.cc`, `arch_aspace_activate`;
`arch/riscv/chip/virt_rv64/chip_virt_rv64.cc`, `arch_timer_arm`

The 64-bit port answers the same questions differently, because it runs the kernel in
**supervisor mode** and isolates with **page tables** instead of PMP. Read
[The MMU](10-the-mmu.md) for the page-table side; here is how the RISC-V parts differ from the
32-bit walk above.

- **Boot.** `_start` still begins in machine mode, but only to hand over. It installs a
  machine trap vector, grants supervisor mode the memory through PMP, delegates the exceptions
  and interrupts the kernel handles to supervisor mode (`medeleg`, `mideleg`), builds the boot
  page tables, writes `satp` (the page-table root register), and `mret`s into supervisor mode at
  a high virtual address.
- **The trap.** `stvec` is in direct mode: one entry, `kickos_rv64_stvec`. It swaps with
  `sscratch` the way Stop 5 swaps with `mscratch`.
- **The syscall.** No trampoline: `syscall_dispatch` is called straight from the trap, with
  supervisor interrupts kept off for the whole dispatch. That keeps every interrupt frame a
  resumable thread context; the one place an interrupt is let in is `arch_irq_window`.
- **The switch.** `arch_switch` does not defer. Outside an interrupt it calls
  `kickos_rv64_switch_now`, which saves a frame on the current stack and restores the incoming
  one directly. Inside an interrupt it only books the target, and the interrupt's exit path
  makes the swap.
- **The timer.** QEMU's core implements the Sstc extension, so supervisor mode writes its own
  comparator, `stimecmp`, and no machine-mode code stays resident.
- **The address space.** `arch_aspace_activate` writes `satp` with the task's root and an
  address-space identifier when the space changes.

**Try it:** the `qemu-riscv64-smp` build, four harts:
```
qemu-system-riscv64 -M virt -bios none -smp 4 -nographic -semihosting \
    -kernel build/qemu-riscv64-smp/user/apps/common/hello/hello -gdb tcp::1234 -S &
```
`break kickos_rv64_switch_now`, `continue`:
```
(gdb) p $priv
$1 = 1
(gdb) info registers satp stvec
satp   0x8000000000080014
stvec  0xffffffff800013e0
```
Supervisor mode. `satp` MODE (bits 63:60) is 8, Sv39 paging, with the root table at physical
page `0x80014`. Then `break syscall_dispatch`, `continue`:
```
(gdb) p/x $scause
$2 = 0x8
(gdb) bt 2
#0  syscall_dispatch (nr=27, ...) at .../kernel/syscall/syscall.cc
#1  0xffffffff80001616 in kickos_rv64_stvec () at .../arch/riscv/rv64imac/switch.S
```
The dispatch is called from the trap vector itself, with no trampoline between.

**Why, and the contract:** [Reference: porting](../reference/porting.md) (the rv64imac rows),
[Book: Porting a new ISA](../book/porting-a-new-isa-riscv.md) (its scope note)

## Where to next

- [15. x86_64](15-x86_64.md): the next chapter, a PC, where firmware hands over a CPU already in
  64-bit mode.
- [10. The MMU](10-the-mmu.md): page tables, address spaces and the user copy, on `qemu-arm64`.
- [11. Multicore](11-multicore.md): what the four harts of `qemu-riscv64-smp` share.
- [12. ARMv6-M](12-armv6m.md) and [13. ARMv8-M](13-armv8m.md): the ARM variants.

## Exercises

1. Stop 6 says one frame serves every switch. Catch the msip trap from a thread preempted in
   user code (not one blocked in a syscall): what are `MPP` and `mepc` now, and which stack is
   the frame on? Use `break trap_entry if $mcause == 0x80000003 && ($mstatus & 0x1800) == 0`.
2. In Stop 10, entry 0 is 256 KiB. Find the image's code size (`riscv64-none-elf-size` on the
   ELF) and explain why the region is larger than the code. What does the thread gain access to
   that it did not need?
3. Stop 5 refuses a user `sp` outside the thread's stack. Predict what happens if a thread sets
   `sp` to a kernel address and then traps: is anything written through that `sp`, and who
   keeps running? Check it with the `faultsurvive_kwrite` image
   (`build/qemu-riscv/user/apps/common/faultsurvive/`) and
   `break kickos_rv_contain_wild_sp`.
