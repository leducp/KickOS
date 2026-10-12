<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 1. Power-on: from reset to the first instruction of `main`

This chapter follows one image from the moment the core leaves reset to the first instruction
of the application's `main`, which runs unprivileged. The image is `hello`
(`user/apps/common/hello/main.cc`): two threads that play ping-pong on two semaphores. Its
plain-C twin `hello_c` takes exactly the same path; only the application code at the very end
differs.

On the way you will see the vector table, the C runtime set-up, the clocks and the early
console, the kernel building its own objects, the first two threads being fabricated, the
first context switch, the MPU being switched on, and then the user-mode runtime: constructors,
the C library, the init that reads the composition, and finally `main`. The concepts behind
each step are in the Book, mainly
[Interrupts and traps](../book/interrupts-and-traps.md),
[Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md)
and [Privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md).

The walk runs on the `qemu` preset (QEMU `mps2-an386`, a Cortex-M4F with a PMSAv7 MPU). Where
the real `f411disco` board (STM32F411, also a Cortex-M4F) does something different, the stop
says so. Start QEMU halted and attach gdb as shown in [0. Setup](00-setup.md). Every **Try
it** below starts from a fresh QEMU.

A note on the two names for the kernel's state: the kernel keeps all of its objects in one
`Kernel` structure, reached in C++ through `kickos::kernel()`. That function is inline, so gdb
cannot call it. In gdb, use the instance directly:

```
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
```

### Stop 1: the vector table gives the core its first stack and its first instruction

**Where:** `arch/arm/chip/mps2/startup.S`, `g_isr_vector`; `arch/arm/common/core_vectors.inc`,
`KICKOS_CORE_VECTORS_V7M`; `arch/arm/chip/mps2/mps2.ld`

A Cortex-M core does not start by executing address 0. It starts by *reading* it. The first
word of the vector table is the initial value of the main stack pointer (MSP); the second word
is the address of the reset handler. The core loads both, then jumps. So before a single
instruction runs, the core already has a stack and knows where the code starts.

The table is plain data in a section called `.isr_vector`. The linker script places that
section first in flash (`.isr_vector ORIGIN(FLASH)`), at address 0 on this machine. The first
16 words are the same on every ARMv7-M chip, so they come from one shared macro. After them come
the device interrupt lines: on MPS2 all 48 point at one generic entry, `kickos_arm_default_irq`,
which turns an interrupt into a kernel event ([07](07-interrupt-to-a-driver.md)).

```
    .section .isr_vector, "a"
    .align 8                         /* 256-byte aligned (VTOR requirement) */
    .global g_isr_vector
g_isr_vector:
    KICKOS_CORE_VECTORS_V7M

    /* External interrupts 0..47 -> the generic IRQ-as-event entry. */
    .rept KICKOS_MAX_IRQ
    .word kickos_arm_default_irq
    .endr
```

```
    .macro KICKOS_CORE_VECTORS_V7M
    .word _estack                    /* 0: initial MSP */
    .word Reset_Handler              /* 1: reset */
    // ...
    .word SVC_Handler                /* 11: SVCall */
    // ...
    .word PendSV_Handler             /* 14: PendSV */
    .word SysTick_Handler            /* 15: SysTick */
    .endm
```

Notice the three kernel entries: `SVC_Handler` (the syscall trap, [02](02-first-syscall.md)),
`PendSV_Handler` (the context switch, Stop 13 below) and `SysTick_Handler` (the one-shot timer,
[03](03-sleep.md)). `_estack` is a linker symbol: the end of RAM.

**f411disco:** `arch/arm/chip/stm32f411/startup.S` uses the same macro and 86 device lines.
One line is different: the TIM2 interrupt goes to `kickos_tim2_clock_isr`, which keeps the
clock counting (Stop 11). The table sits at the start of internal flash, `0x08000000`, which
the chip aliases to address 0 at boot.

**Try it:** attach gdb to a QEMU started with `-S`. Nothing has run yet.

```
(gdb) info registers pc sp
pc             0x1be8              0x1be8 <Reset_Handler()>
sp             0x20400000          0x20400000
(gdb) x/2wx 0
0x0:	0x20400000	0x00001be9
```

The two words at address 0 are exactly SP and PC. The PC word is `0x1be9`, odd: bit 0 set means
"Thumb code", the only instruction set a Cortex-M runs. The core drops that bit when it loads
PC. `0x20400000` is the top of the 4 MiB SRAM.

**Why, and the contract:** [Book: interrupts and traps](../book/interrupts-and-traps.md),
[Book: the machine underneath](../book/the-machine-underneath.md),
[Reference: adding a board/chip](../reference/porting.md#adding-a-boardchip-the-seven-edit-points)

### Stop 2: the reset handler sets up the C runtime

**Where:** `arch/arm/chip/mps2/chip_mps2.cc`, `Reset_Handler`; `arch/common/startup_ranges.cc`,
`kickos_ranges_init`; `arch/common/crt_tail.h`, `kickos_crt_tail`

At reset, RAM holds nothing useful. C code assumes two things before it starts: every global
with an initial value holds that value (`.data`), and every other global is zero (`.bss`).
Nobody has done that yet, so the reset handler's first job is to do it. Until then, no code
may read a global.

```c++
void Reset_Handler(void)
{
    kickos_ranges_init();
    // Enable the FPU BEFORE running static constructors: with the hard-float ABI
    // the compiler may emit FP instructions in a global initializer, which would
    // UsageFault (CP10/CP11 disabled at reset) -> HardFault before kmain.
    kickos_armv7m_enable_fpu();
    kickos_crt_tail();
}
```

`kickos_ranges_init` does not know section names. The linker script writes two small tables
into flash: a *copy table* of `{source, destination, length}` triples and a *zero table* of
`{destination, length}` pairs. The function walks them.

```c++
        for (uint32_t* e = __kickos_copy_table_start; e < __kickos_copy_table_end; e += 3)
        {
            uint32_t const* src = reinterpret_cast<uint32_t const*>(e[0]);
            uint32_t* dst = reinterpret_cast<uint32_t*>(e[1]);
            uint32_t const words = e[2] / 4u; // ranges are word-sized (linker ALIGN(4))
            for (uint32_t i = 0; i < words; i++)
            {
                dst[i] = src[i];
            }
        }
        // ...
```

Why tables, and not one `.data` and one `.bss`? Because there are two of each. The kernel's
globals live at the bottom of RAM. The application's globals (and libc's, and the heap) live in
a separate window, `.appdata` and `.appbss`. The MPU will later give unprivileged code that
window and nothing else, so kernel variables must not sit inside it.

**Try it:** `SystemCoreClock` is a kernel global with an initial value of 25 MHz.

```
(gdb) p SystemCoreClock
$1 = 0
(gdb) x/6wx &__kickos_copy_table_start
0x10c4c:	0x00020000	0x20010000	0x00000580	0x00010c74
0x10c5c:	0x20000000	0x00000054
(gdb) x/4wx &__kickos_zero_table_start
0x10c64:	0x20010580	0x0000fa80	0x20000060	0x00007af0
(gdb) next
(gdb) p SystemCoreClock
$2 = 25000000
```

Read the tables: copy `0x580` bytes from flash `0x20000` to `0x20010000` (the app's data), copy
`0x54` bytes from `0x10c74` to `0x20000000` (the kernel's data); zero `0xfa80` bytes at
`0x20010580` (the app's bss, heap included) and `0x7af0` bytes at `0x20000060` (the kernel's
bss). After `next`, the global holds its value.

**Why, and the contract:** [Book: where your RAM goes](../book/where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md)
(the copy and zero tables, and the split),
[Reference: memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 3: the FPU is switched on, and the kernel runs no constructors

**Where:** `arch/arm/common/regs.h`, `kickos_armv7m_enable_fpu`; `arch/common/crt_tail.h`,
`kickos_crt_ctors`; `arch/arm/common/cortexm.ld.h`, `KICKOS_CORTEXM_CTORS`;
`arch/common/sections.ld.h`, `KICKOS_KERNEL_CTORS_NONE_ASSERT`

The Cortex-M4F has a floating-point unit, but it is off at reset: any FP instruction would
fault. The compiler may use FP registers in almost any function, including a global's
initialiser, so the reset handler enables the coprocessor (two bit pairs in `CPACR`) before
any constructor runs. On QEMU that happens right after the copy loops of Stop 2.

Then `kickos_crt_tail` runs the C++ constructors, calls `arch_init`, and calls `kmain`:

```c++
inline __attribute__((always_inline)) void kickos_crt_tail(void)
{
    kickos_crt_ctors();
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}
```

`kickos_crt_ctors` walks `__init_array_start` to `__init_array_end`. In a KickOS image this list
is **empty**, and the linker refuses an image where it is not. The linker script splits
constructors by archive: only the kernel's own archives may land in `.init_array`, and the
kernel declares its globals `constinit`, so they need no code to initialise.
Every other constructor (the application's, libstdc++'s, newlib's) goes to a second list,
`.kickos_app_init_array`. That list runs much later, in user mode (Stop 15). A constructor run
here would run privileged, before the kernel exists, and could not make a syscall.

**f411disco:** `Reset_Handler` in `arch/arm/chip/stm32f411/chip_stm32f411.cc` enables the FPU
*first*, then calls `kickos_ranges_init`, and spells the constructor loop out instead of
calling `kickos_crt_tail`. The result is the same.

**Try it:**

```
(gdb) x/wx 0xE000ED88
0xe000ed88:	0x00000000
(gdb) p &__init_array_start
$1 = (void (*(*)[])(void)) 0x109c0
(gdb) p &__init_array_end
$2 = (void (*(*)[])(void)) 0x109c0
(gdb) break arch_init
Breakpoint 1 at 0x0: arch_init. (2 locations)
(gdb) continue
Breakpoint 1.2, arch_init () at .../arch/arm/chip/mps2/chip_mps2.cc:47
(gdb) x/wx 0xE000ED88
0xe000ed88:	0x00f00000
```

`0xE000ED88` is `CPACR`; `0x00f00000` is full access to CP10 and CP11, the FPU. The two
`__init_array` bounds are equal: zero kernel constructors. gdb finds two locations for
`arch_init`; the one at address 0 is left-over debug information from code the linker
discarded, and is never hit.

**Why, and the contract:** [Book: what's under #include](../book/whats-under-include-libc-and-the-cxx-runtime.md)
(global constructor order on a freestanding OS),
[Reference: `ctors-run-before-init-entry`](../reference/invariants.md#app-entry--init-seam)

### Stop 4: `arch_init` brings up the chip: clocks and core priorities

**Where:** `arch/arm/chip/mps2/chip_mps2.cc`, `arch_init`; `arch/arm/armv7m/arch_armv7m.cc`,
`kickos_armv7m_init`; f411: `arch/arm/chip/stm32f411/chip_stm32f411.cc`, `arch_init`,
`clock_init`, `tim2_clock_init`, `usart2_init`

`arch_init` is the chip's one bring-up function. The kernel calls it once, before `kmain`, and
expects the hardware to be ready afterwards: the CPU at its final speed, the console able to
print, the time base counting.

On QEMU there is little to do. The emulated core has no PLL to configure, so `SystemCoreClock`
is a constant 25 MHz. The console and the clock both go through semihosting, which needs no
set-up. What remains is the part every ARMv7-M chip shares, `kickos_armv7m_init`:

```c++
void kickos_armv7m_init(void)
{
    // SHPR2[31:24] = SVCall (#11); SHPR3[23:16] = PendSV (#14), [31:24] = SysTick.
    reg32(SCB_SHPR2) = (reg32(SCB_SHPR2) & 0x00FFFFFFu) | (PRIO_SVCALL << 24);
    uint32_t shpr3 = reg32(SCB_SHPR3) & 0x0000FFFFu;
    shpr3 |= (PRIO_PENDSV << 16) | (PRIO_SYSTICK << 24);
    reg32(SCB_SHPR3) = shpr3;
    // ...
}
```

These priorities matter for everything that follows. On a Cortex-M, a lower number is a more
urgent priority. The kernel's critical section raises `BASEPRI` to `0x20`, which blocks every
exception at `0x20` or below in urgency. SVCall and SysTick sit at `0xE0` and PendSV at `0xF0`,
the least urgent of all. So PendSV, the context switch, runs only after every other exception
is done. That is what makes the switch safe to request from anywhere.

**f411disco:** here `arch_init` does real work, in a fixed order. `clock_init` widens the
flash wait states, starts the external crystal (HSE), programs the PLL and switches the system
clock to it; only then does `SystemCoreClock` change. `tim2_clock_init` starts the 32-bit TIM2
counter that the clock reads. `usart2_init` programs the console UART, whose baud rate depends
on the final bus clock, which is why the clock comes first.

**Try it:**

```
(gdb) break kickos_armv7m_init
(gdb) continue
(gdb) finish
(gdb) x/wx 0xE000ED1C
0xe000ed1c:	0xe0000000
(gdb) x/wx 0xE000ED20
0xe000ed20:	0xe0f00000
(gdb) p SystemCoreClock
$1 = 25000000
```

`SHPR2` holds SVCall's priority in its top byte (`0xe0`); `SHPR3` holds SysTick (`0xe0`) and
PendSV (`0xf0`).

**Why, and the contract:** [Book: interrupts and traps](../book/interrupts-and-traps.md)
(masking), [Reference: critical section, timer, clock, NVIC](../reference/porting.md#critical-section-timer-clock-nvic)

### Stop 5: `kmain` starts, and the banner goes out through the early console

**Where:** `kernel/init/kmain.cc`, `kmain`, `kbanner`; `kernel/init/console.cc`, `kputs`,
`console_write_line_sync`; `kernel/init/console_tx.cc`, `console_tx_insert_line`;
`arch/common/semihost_console.cc`, `arch_console_write_sync`

`kmain` is the kernel's `main`. It runs on the boot stack (MSP), privileged. On QEMU no
interrupt source is enabled yet. Its first lines publish `argc`/`argv` for
the init (into app-side storage, because the init will read them unprivileged), then print the
banner, then build the kernel's objects:

```c++
        kdiag_led_init();
        kbanner();
        sched::init();
        domain_init();
        task_init();
        grant_reserved_validate();
        irq_init();            // before any driver attaches
        console_buffer_init(); // after irq_init
        ktrace_init();
```

The banner prints *before* the console's buffer exists. `kputs` normally copies text into a
ring that an interrupt drains in the background. The ring is not armed yet, so
`console_tx_insert_line` falls back to `console_write_line_sync`: it writes the line
synchronously through the chip's `arch_console_write_sync`, with interrupts masked. That slow
path is also what a panic uses, so the kernel can always print.

On QEMU, `arch_console_write_sync` hands each byte to the host with the semihosting call
`SYS_WRITEC`. **f411disco:** it writes the USART2 data register directly, waiting for the
transmit-empty flag.

**Try it:**

```
(gdb) break arch_console_write_sync
(gdb) continue
(gdb) bt
#0  arch_console_write_sync (buf=... "\r\n...", n=2) at .../arch/common/semihost_console.cc:37
#1  write_line_polled (buf=0xf293 "\n", n=1) at .../kernel/init/console.cc:357
#2  console_write_line_sync (buf=0xf293 "\n", n=1) at .../kernel/init/console.cc:377
#3  console_tx_insert_line (buf=0xf293 "\n", n=1, crlf=1) at .../kernel/init/console_tx.cc:264
#4  kickos::console_emit (...) at .../kernel/init/console.cc:421
#5  kickos::kconsole_write_impl (...) at .../kernel/init/console.cc:479
#6  kickos::kputs (s=s@entry=0xf293 "\n") at .../kernel/init/console.cc:549
#7  kickos::(anonymous namespace)::kbanner () at .../kernel/init/kmain.cc:94
#8  kickos::kmain (argc=argc@entry=0, argv=argv@entry=0x0) at .../kernel/init/kmain.cc:247
#9  kickos_crt_tail () at .../arch/common/crt_tail.h:33
#10 Reset_Handler () at .../arch/arm/chip/mps2/chip_mps2.cc:117
(gdb) p buf[0]@n
$1 = "\r\n"
```

The first byte the kernel ever sends is the banner's leading newline, turned into `\r\n` on
the way out. Read the stack from the bottom up: it is the whole boot path so far.

**Why, and the contract:** [Reference: console](../reference/console.md)

### Stop 6: the kernel's objects are already there; `kmain` puts them in their start state

**Where:** `kernel/init/instance.cc`, `g_instance`; `kernel/include/kickos/instance.h`,
`Kernel`, `kernel()`; `kernel/sched/sched.cc`, `sched::init`; `kernel/domain/domain.cc`,
`domain_init`; `kernel/task/task.cc`, `task_init`; `kernel/irq/irq.cc`, `irq_init`;
`kernel/init/console_tx.cc`, `console_buffer_init`

The kernel allocates nothing at run time for its own bookkeeping. All of its objects (the
thread slots, the tasks, the memory domains, the interrupt table, the ready queues) are fields
of one `Kernel` structure, and there is one instance of it, `g_instance`, declared `constinit`.
`constinit` makes the compiler prove that the object needs no constructor, so it lives in
`.bss` and is ready the moment Stop 2 zeroed it. That is why Stop 3 could run zero kernel
constructors.

`kmain` then sets the start state of each part:

- `sched::init` clears the running-thread and idle-thread seats.
- `domain_init` resets every **memory domain** (the set of regions a group of threads may
  access) and builds domain 0, the kernel's: privileged, never freed, covering the whole RAM
  arena.
- `task_init` resets every **task** (a group of threads sharing one domain).
- `irq_init` points every interrupt line at its default handler.
- `console_buffer_init` arms the console ring. On QEMU the semihosting backend has no
  interrupt line (`-1`), so the ring is drained by the writers themselves. **f411disco:** the
  backend reports the USART2 line, and the ring is drained by its transmit interrupt.

**Try it:**

```
(gdb) p &kickos::detail::g_instance
$1 = (kickos::InstanceLocal<kickos::Kernel> *) 0x200047b0 <kickos::detail::g_instance>
(gdb) p sizeof(kickos::Kernel)
$2 = 10208
(gdb) break kickos::domain_init
(gdb) continue
(gdb) finish
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) p $k->domains[0].privileged
$3 = true
(gdb) p/x $k->domains[0].regions[0]
$4 = {base = 0x20020000, size = 0x3dc000, attr = 0x3}
```

The whole `Kernel` structure is about 10 KiB. The kernel's `.bss` holds a few other fixed
arrays beside it, such as the per-thread kernel stacks the banner reports (`kstack 1008 B x
17`). The kernel domain covers `0x20020000` to the end of RAM, read and write (`attr = 0x3`, see `ARCH_MPU_R` and
`ARCH_MPU_W` in `arch.h`).

**Why, and the contract:** [Reference: scheduler](../reference/architecture.md#scheduler-the-core-constraint),
[Reference: memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 7: the arena hands out the first two stacks

**Where:** `kernel/init/kmain.cc`, `boot_stack_alloc`; `arch/common/arch_ram_common.cc`,
`arch_ram_alloc`, `arch_ram_base`, `arch_ram_size`

The RAM above the kernel's static data is the **arena**: a block the kernel hands out with a
bump allocator. Nothing is ever given back one piece at a time. The first two pieces are
stacks: one for the **idle** thread (the thread that runs when nothing else can) and one for
**root** (the first user thread). Idle comes first, and the order is fixed: the linker script
replays exactly these two allocations to check at link time that they fit.

```c++
    size_t const rsz = arch_ram_region_size(size);
    size_t const ralign = arch_ram_region_align(size);
    // ...
    uintptr_t const cur = base + g_ram_used;
    // Natural (absolute) alignment: PMSA/NAPOT require base aligned to size.
    uintptr_t const aligned = (cur + (ralign - 1)) & ~static_cast<uintptr_t>(ralign - 1);
    size_t const off = static_cast<size_t>(aligned - base);
    if (aligned >= cur and off <= total and rsz <= total - off)
    {
        p = reinterpret_cast<void*>(aligned);
        g_ram_used = static_cast<uint32_t>(off + rsz);
    }
```

Notice the alignment. A PMSAv7 MPU region must be a power of two in size and start at a
multiple of its size. Root's stack will be one MPU region, so its 8 KiB block must start on an
8 KiB boundary. The allocator rounds up, and the gap it skips is never used.

**Try it:**

```
(gdb) p/x arch_ram_base()
$1 = 0x20020000
(gdb) p/x arch_ram_size()
$2 = 0x3dc000
(gdb) break arch_ram_alloc
(gdb) continue
Breakpoint 1, arch_ram_alloc (size=512) at .../arch/common/arch_ram_common.cc:85
(gdb) finish
Value returned is $3 = (void *) 0x20020000
(gdb) continue
Breakpoint 1, arch_ram_alloc (size=8192) at .../arch/common/arch_ram_common.cc:85
(gdb) finish
Value returned is $4 = (void *) 0x20022000
```

Idle's 512 bytes take the start of the arena. Root's 8 KiB cannot start at `0x20020200`; the
next 8 KiB boundary is `0x20022000`. The 7.5 KiB in between is the price of the MPU's rule.

**Why, and the contract:** [Book: thread stacks](../book/thread-stacks-and-the-kiss-tension.md),
[Reference: thread pointer and kernel stacks](../reference/porting.md#thread-pointer-and-kernel-stacks-what-a-new-arch-owes)

### Stop 8: idle and root are fabricated as if they had already been switched out

**Where:** `kernel/init/kmain.cc`, `idle_build`, `kmain`; `kernel/thread/thread.cc`,
`thread_create`; `arch/arm/armv7m/arch_armv7m.cc`, `arch_context_init`

A context switch saves a thread's registers on its own stack, and later restores them from
there. A brand-new thread has never been switched out, so there is nothing to restore. The
trick is to write, by hand, exactly the frame the switch would have saved. The first switch
then "restores" a thread that never ran, and the core lands at its entry point.

`thread_create` fills the thread control block (name, priority, task, MPU region set) and
calls the arch function `arch_context_init` for the frame:

```c++
    *(--sp) = 0x01000000u;                                    // xPSR (Thumb bit)
    *(--sp) = reinterpret_cast<uint32_t>(entry) & ~1u;        // PC = entry
    *(--sp) = ret;                                            // LR: entry returns here
    *(--sp) = 0;                                              // r12
    *(--sp) = 0;                                              // r3
    *(--sp) = 0;                                              // r2
    *(--sp) = 0;                                              // r1
    *(--sp) = reinterpret_cast<uint32_t>(arg);               // r0 = arg
    // ...
    *(--sp) = 0xFFFFFFFDu; // EXC_RETURN: thread mode, PSP, non-FP frame
    for (int i = 0; i < 8; i++)
    {
        *(--sp) = 0; // r11..r4
    }
    // ...
    ctx->npriv = npriv;
```

The top eight words are what the hardware pushes on an exception. The lower nine are what
`PendSV_Handler` pushes: `r4` to `r11` and the `EXC_RETURN` code that tells the core how to
return. `npriv` is the thread's privilege. **This is where root becomes unprivileged:** `kmain`
sets `root_attr.privileged = false`, so `npriv` is 1 from the first instruction. There is no
later moment where root is demoted. (A thread inside a syscall does run kernel code privileged,
for the length of the call; [02](02-first-syscall.md) shows how. Its resting privilege, the one
`resting_npriv` records, never changes.) Idle is the only privileged thread in the system.

**Try it:**

```
(gdb) break arch_context_init
(gdb) continue
Breakpoint 1, arch_context_init (ctx=0x20004b18 <kickos::detail::g_instance+872>,
    entry=0x6e79 <kickos::(anonymous namespace)::idle_entry(void*)>, arg=0x0,
    stack_base=0x20020000, stack_size=512, privileged=1) at .../arch/arm/armv7m/arch_armv7m.cc:218
(gdb) continue
Breakpoint 1, arch_context_init (ctx=0x20004d40 <kickos::detail::g_instance+1424>,
    entry=0xc651 <kickos_root_entry(void*)>, arg=0x0,
    stack_base=0x20022000, stack_size=8192, privileged=0) at .../arch/arm/armv7m/arch_armv7m.cc:218
(gdb) break kickos::sched::start
(gdb) continue
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) p/x $k->threads.slots[0].ctx
$1 = {sp = 0x20023fbc, npriv = 0x1, resting_npriv = 0x1, stack_lo = 0x20022000,
      stack_hi = 0x20024000, kernel_sp = 0x20000450}
(gdb) x/17wx $k->threads.slots[0].ctx.sp
0x20023fbc:	0x00000000	0x00000000	0x00000000	0x00000000
0x20023fcc:	0x00000000	0x00000000	0x00000000	0x00000000
0x20023fdc:	0xfffffffd	0x00000000	0x00000000	0x00000000
0x20023fec:	0x00000000	0x00000000	0x00001833	0x0000c650
0x20023ffc:	0x01000000
```

Read the dump from low to high: eight zeros (`r4`-`r11`), `EXC_RETURN` `0xfffffffd`, then the
hardware frame: `r0`-`r3` and `r12` all 0, LR `0x1833`, PC `0xc650`, xPSR `0x01000000`. PC is
`kickos_root_entry`. LR is `kickos_user_thread_return` (with the Thumb bit): an unprivileged
thread whose entry function returns lands there, and it exits through a syscall, since such a
thread cannot do the kernel's exit work itself. Root lives in thread slot 0.
`kernel_sp` is the top of root's own kernel stack, where its syscalls will run
([02](02-first-syscall.md)).

**Why, and the contract:** [Book: context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md),
[Reference: `switch-frame-matches-init`](../reference/invariants.md#context-switch--fp-saverestore),
[Reference: `root-unprivileged-idle-alone-privileged`](../reference/invariants.md#syscall-privilege--the-userkernel-boundary)

### Stop 9: root's memory regions are composed once, at creation

**Where:** `kernel/thread/thread.cc`, `thread_regions_boot`, `thread_regions_assemble`;
`kernel/include/kickos/mpuset.h`, `MpuSet`

Each thread carries its own MPU region set (an `MpuSet`), composed when the thread is created.
The switch only copies it into the hardware (Stop 13). For root on this board the set has
three regions:

1. the whole flash, read and execute: root fetches its code and reads constants;
2. the app data window, read and write: `.appdata`, `.appbss`, the C library's state and the
   heap;
3. root's own stack, read and write.

Nothing else. In particular the kernel's `.data` and `.bss` (at `0x20000000`) and every other
thread's stack are outside all three. `MpuSet` also keeps the regions already encoded in the
MPU's register format (`image_`), so the switch writes words and computes nothing.

**Try it:** at `kickos::sched::start`, as in Stop 8:

```
(gdb) p/x $k->threads.slots[0].mpu
$1 = {... regions_ = {{base = 0x0, size = 0x400000, attr = 0x5},
      {base = 0x20010000, size = 0x10000, attr = 0x3},
      {base = 0x20022000, size = 0x2000, attr = 0x3}, ...}, count_ = 0x3, ...
      image_ = {rbar = {0x0, 0x20010000, 0x20022000, 0x0, ...},
                rasr = {0x603002b, 0x1303001f, 0x13030019, 0x0, ...}}}
```

`attr = 0x5` is read and execute, `0x3` read and write. In the encoded `rasr` words you can
see the size field (bits 5:1: `0x15` for 4 MiB, `0xf` for 64 KiB, `0xc` for 8 KiB) and, on the
two data regions, bit 28: execute-never.

**Why, and the contract:** [Book: protection follows the CPU, not the scheduler's intent](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Reference: memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 10: root receives its capability table and its authority

**Where:** `kernel/init/kmain.cc`, `kmain`; `kernel/syscall/cap.cc`, `cap_slab_init`,
`cap_slab_attach`, `cap_seat_authority`, `cap_libc_lock_take`, `cap_seat_libc_lock`

A user thread can name a kernel object only through a **capability**: an entry in the thread's own
capability table that names the object and the rights the thread has over it. User code holds a
**handle**, the number that picks the entry. Root gets a table with room for
`KICKOS_MAX_HANDLES` entries (16 here) out of a shared slab (`cap_slab_attach`).

Two things are seated before root runs:

- **The authority word** (`cap_seat_authority`), a set of bits for acts that name no object:
  shutting down the system, creating tasks, reserving memory. Root gets all of them
  (`CAP_AUTH_ALL`). The init will hand each task only the bits its composition lists.
- **The libc lock** (`cap_libc_lock_take`, `cap_seat_libc_lock`): a kernel mutex seated at a
  fixed handle. newlib calls a lock around `malloc` and stdio; the user-side glue
  (`user/src/newlib_lock.cc`) implements that lock with this mutex.

The comment in `kmain` gives the order: after `thread_create`, which zeroes the table, and
before `sched::start`, so the entries exist before root's first instruction.

**Try it:**

```
(gdb) break kickos::cap_seat_authority
(gdb) continue
Breakpoint 1, kickos::cap_seat_authority (t=0x20004d40 <kickos::detail::g_instance+1424>,
    auth=auth@entry=255) at .../kernel/syscall/cap.cc:1091
(gdb) set $k = &kickos::detail::g_instance.per_instance[0]
(gdb) p/x $k->threads.slots[0].authority
$1 = 0x0
(gdb) finish
(gdb) p/x $k->threads.slots[0].authority
$2 = 0xff
```

**Why, and the contract:** [Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Book: authority from possession or from identity](../book/authority-from-possession-or-from-identity.md),
[Reference: object model, capabilities and IPC](../reference/architecture.md#object-model-capabilities--ipc)

### Stop 11: the time base and the timer at start: nothing to wait for yet

**Where:** `kernel/sched/sched.cc`, `sched::start`; `kernel/time/time.cc`, `ktime_rearm`;
`arch/arm/common/arch_arm_common.cc`, `arch_timer_arm`, `arch_timer_disarm`;
`arch/arm/chip/mps2/chip_mps2.cc`, `arch_clock_now`

KickOS is **tickless**: there is no periodic timer interrupt. There are two separate pieces of
hardware behind time:

- a **clock**, `arch_clock_now`, which returns nanoseconds since boot and never goes back;
- a **one-shot timer**, `arch_timer_arm(deadline)`, which interrupts once at a deadline. On
  every Cortex-M chip this is SysTick (`arch/arm/common/arch_arm_common.cc`).

Every time the scheduler picks a thread, `ktime_rearm` computes the nearest deadline: the first
sleeper, or the running thread's time slice. If there is none, it disarms the timer. At
`sched::start` nothing sleeps and root is FIFO (no slice), so the timer is switched off. The
first arm happens when the first thread sleeps ([03](03-sleep.md)).

The clock is a per-chip choice. On QEMU, `arch_clock_now` asks the host through semihosting
(`SYS_CLOCK`), which counts in hundredths of a second, so the clock moves in 10 ms steps.
**f411disco:** the clock is TIM2, a 32-bit counter at the core frequency, extended to 64 bits
in software (`tim2_ticks`); the TIM2 interrupt from Stop 1 notices each wrap.

**Try it:**

```
(gdb) break arch_timer_disarm
(gdb) break arch_timer_arm
(gdb) continue
Breakpoint 1, arch_timer_disarm () at .../arch/arm/common/arch_arm_common.cc:199
(gdb) bt 3
#0  arch_timer_disarm () at .../arch/arm/common/arch_arm_common.cc:199
#1  kickos::sched::start () at .../kernel/include/kickos/held.h:24
#2  kickos::kmain (argc=argc@entry=0, argv=argv@entry=0x0) at .../kernel/init/kmain.cc:429
```

The first timer call is a disarm, from `sched::start`. Frame #1 names a header line, not
`sched.cc`, because `ktime_rearm` and its helpers were inlined into `sched::start`.

**Why, and the contract:** [Book: the time base](../book/the-time-base-a-monotonic-clock-you-can-trust.md),
[Reference: tickless timer and clock](../reference/invariants.md#tickless-timer--clock)

### Stop 12: `sched::start` picks root and asks for the first switch

**Where:** `kernel/sched/sched.cc`, `sched::start`; `arch/arm/armv7m/switch.S`, `arch_start`

`kmain` has added idle and root to the ready queues. `sched::start` takes the kernel lock (an
`IrqLock`, which raises `BASEPRI`), picks the most urgent ready thread (root, at priority 31;
idle is always last), records it as running, stashes its MPU set, sets the timer, and hands
over to the arch:

```c++
            Thread* first = policy_pick_next();
            seat_running(first, me);
            // ...
            [[maybe_unused]] struct arch_aspace* const rspace = aspace_activate_for(first);
            first->mpu.apply();
            // ...
            ktime_rearm(first, lock);
            // ...
            arch_start(&kernel().boot(me), &first->ctx);
```

`first->mpu.apply()` does **not** touch the MPU. It only records which region set the next
switch must load (`arch_mpu_apply` in `arch/common/arch_mpu_stash.cc`). On ARMv7-M,
`arch_start` does not switch either. It reuses the normal switch path: it marks "no current
thread", names root as the next one, and pends PendSV.

```
arch_start:                         /* r0=boot (ignored on MCU), r1=first */
    ldr     r2, =g_arch_current
    movs    r3, #0
    str     r3, [r2]                /* g_arch_current = 0 (first-start sentinel) */
    ldr     r2, =g_arch_next
    str     r1, [r2]                /* g_arch_next = first */
    ldr     r2, =0xE000ED04         /* SCB->ICSR */
    ldr     r3, =0x10000000         /* PENDSVSET */
    str     r3, [r2]
    dsb
    isb
    bx      lr
```

PendSV is now pending, but `BASEPRI` is still `0x20`, which holds it back. `arch_start`
returns into `sched::start`; when the lock's destructor writes `BASEPRI` back to 0, PendSV
fires at once. The boot stack frame below it is abandoned for good.

**Try it:**

```
(gdb) break arch_start
(gdb) continue
(gdb) info registers r1 msp psp control basepri
r1             0x20004d40          536890688
msp            0x203fff80          541065088
psp            0x0                 0
control        0x0                 0
basepri        0x20                32
(gdb) x/wx 0xE000ED94
0xe000ed94:	0x00000000
(gdb) stepi 11
(gdb) x/i $pc
=> 0x3c8c <_ZN6kickos5sched5startEv+80>:	msr	BASEPRI, r5
(gdb) x/wx 0xE000ED04
0xe000ed04:	0x1000e800
```

`r1` is root's context (`0x20004d40`, the `ctx` you saw in Stop 8). `PSP` is still 0: no thread
has run. `MPU_CTRL` (`0xE000ED94`) is 0: the MPU is still off. After the eleven instructions of
`arch_start` the core is back in `sched::start`, about to lower `BASEPRI`. `ICSR` shows
`PENDSVSET` (bit 28) and, in its low bits, the pending exception number `0xe` = 14, PendSV.

**Why, and the contract:** [Reference: context switch (PendSV) and first-thread start](../reference/porting.md#context-switch-pendsv--first-thread-start),
[Reference: `arch-switch-may-defer`](../reference/invariants.md#deferred-switch--blocking-model)

### Stop 13: the first PendSV switches to root and turns the MPU on

**Where:** `arch/arm/armv7m/switch.S`, `PendSV_Handler`;
`arch/arm/common/kickos_arch_mpu_commit_default.cc`, `kickos_arch_mpu_commit`;
`arch/arm/common/arch_arm_mpu_pmsav7.cc`, `kickos_arm_mpu_program`

`PendSV_Handler` is the context switch. Normally it first saves the outgoing thread. Here the
outgoing context is null (the sentinel from `arch_start`), so it skips the save and goes
straight to the restore half:

```
    ldr     r1, =g_arch_next
    ldr     r1, [r1]                /* incoming ctx* */
    str     r1, [r3]                /* g_arch_current = incoming */
    ldr     r0, [r1]                /* incoming->sp */
    ldmia   r0!, {r4-r11, lr}       /* pop callee regs + EXC_RETURN */
    // ...
    msr     psp, r0
    ldr     r2, [r1, #4]            /* incoming->npriv */
    mrs     r3, control
    bfi     r3, r2, #0, #1          /* CONTROL.nPRIV = incoming npriv */
    msr     control, r3
    isb
    // ...
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    pop     {r0, lr}
    cpsie   i                       /* pair consumed; never resume a thread masked */
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */
```

It pops the nine words Stop 8 fabricated, points `PSP` at the hardware frame, and sets
`CONTROL.nPRIV` to root's 1. Then it programs the MPU for the incoming thread. The order is the
point: the regions are loaded only after the registers are swapped, so no thread ever runs under
another thread's regions ([Book](../book/protection-follows-the-cpu-not-the-schedulers-intent.md)
tells the bug this prevents).

`kickos_arm_mpu_program` keeps a copy of what the MPU holds, and on later switches writes only
the regions that changed. This first time the copy is not valid yet, so it writes every
region, and then enables the MPU:

```c++
    // Do not drop this because kickos_arm_mpu_fixed_init also enables the MPU: only imxrt1062
    // calls that, so on every other PMSAv7 chip this is the ONLY write that enables it. It
    // enables, so unlike a leading MPU_CTRL = 0 it cannot stop the chip fixed rows applying.
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
```

`PRIVDEFENA` keeps the default memory map for privileged code, so the kernel still reaches
everything; unprivileged code reaches only the programmed regions. **This is the instant the
MPU switches on**, and it is during the first switch, not during boot: the boot code never
runs under it.

Finally `bx lr` with `EXC_RETURN = 0xfffffffd` makes the core pop the hardware frame from
`PSP` and return to thread mode, unprivileged, at root's entry.

**f411disco:** identical. The STM32F411 has the same PMSAv7 MPU and runs the same code. (The chips
whose protection unit is not PMSAv7 differ here: `mk64f` defines its own
`kickos_arch_mpu_commit`, and the PMSAv8 parts take the one in
`arch/arm/common/arch_arm_pmsav8.cc`; see [13](13-armv8m.md).)

**Try it:**

```
(gdb) break PendSV_Handler
(gdb) break kickos_arm_mpu_program
(gdb) continue
Breakpoint 1, PendSV_Handler () at .../arch/arm/armv7m/switch.S:108
(gdb) info registers xpsr msp psp control lr
xpsr           0x6100000e          1627389966
msp            0x203fff60          541065056
psp            0x0                 0
control        0x0                 0
lr             0xfffffff9          -7
(gdb) p g_arch_current
$1 = {v_ = {_M_b = {_M_p = 0x0}}}
(gdb) continue
Breakpoint 2, kickos_arm_mpu_program (img=0x20004e1c <kickos::detail::g_instance+1644>) at
    .../arch/arm/common/arch_arm_mpu_pmsav7.cc:87
(gdb) x/wx 0xE000ED94
0xe000ed94:	0x00000000
(gdb) finish
(gdb) x/wx 0xE000ED94
0xe000ed94:	0x00000005
```

The low byte of `xpsr` is the active exception: `0xe`, PendSV. `lr = 0xfffffff9` says the
exception interrupted thread mode running on `MSP`: the boot code in `sched::start`.
`MPU_CTRL` goes from 0 to 5: `ENABLE` (bit 0) and `PRIVDEFENA` (bit 2).

**Why, and the contract:** [Book: protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Reference: `mpu-apply-on-every-switch-in`](../reference/invariants.md#syscall-privilege--the-userkernel-boundary),
[Reference: context switch (PendSV) and first-thread start](../reference/porting.md#context-switch-pendsv--first-thread-start)

### Stop 14: root's first instruction, unprivileged

**Where:** `user/src/root_entry.cc`, `kickos_root_entry`; `system/include/kickos/sys/init.h`

The first instruction any unprivileged thread executes is the first instruction of
`kickos_root_entry`. It is not kernel code. It lives in the user library
(`libkickos_user.a`), and `init.h` explains why it must: root is unprivileged from its first
instruction, so on boards with an MMU the kernel's text is not even mapped for it.

**Try it:**

```
(gdb) break kickos_root_entry
(gdb) continue
Breakpoint 1, kickos_root_entry () at .../user/src/root_entry.cc:52
(gdb) info registers pc sp msp psp control basepri
pc             0xc650              0xc650 <kickos_root_entry(void*)>
sp             0x20024000          0x20024000
msp            0x203fff60          541065056
psp            0x20024000          537018368
control        0x3                 3
basepri        0x0                 0
(gdb) p kickos::detail::g_instance.per_instance[0].live
Cannot access memory at address 0x200048bc
```

`CONTROL = 3`: bit 0 (`nPRIV`) says unprivileged, bit 1 (`SPSEL`) says the thread stack, `PSP`.
`PSP` is `0x20024000`, the top of root's stack: the hardware frame has been popped. And gdb
itself can no longer read the kernel's memory. QEMU checks the debugger's reads against the MPU
as the core sees it now, and the kernel's `.bss` is in none of root's three regions. The
isolation is real, and you just watched it refuse.

**Why, and the contract:** [Book: privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md),
[Reference: user/kernel separation](../reference/architecture.md#userkernel-separation)

### Stop 15: the C and C++ runtime starts in user mode

**Where:** `user/src/root_entry.cc`, `kickos_root_entry`; `arch/arm/common/cortexm.ld.h`,
`.kickos_app_init_array`; `user/src/newlib_reent.cc`, `s_reent`; `user/src/newlib_sbrk.cc`,
`_sbrk`; `user/src/newlib_lock.cc`; `kernel/thread/reent.cc`, `reent_seam_read`

Root now does what a normal C runtime does before `main`: it runs the constructors. These are
the ones Stop 3 kept out of the kernel's list.

```c++
    for (void (**fn)() = __kickos_app_init_array_start; fn != __kickos_app_init_array_end;
         fn++)
    {
        (*fn)();
    }
    int const status = kickos_init_entry(kickos_init_args.argc, kickos_init_args.argv);
```

They run unprivileged, on root's stack, at root's priority 31, one after another, with the
kernel fully alive. So a constructor may make a syscall. In `hello` there are three, none from
the application itself: newlib's `register_fini`, `kickos_exception_report_install`
(`system/cxx/exception_report.cc`) and libstdc++'s emergency exception-buffer pool.

That last one reads an environment variable (`getenv("GLIBCXX_TUNABLES")`) and then calls
`malloc` for its buffer. Those two calls bring in the rest of the C library:

- **The C library state.** newlib keeps per-thread state (`errno`, stdio buffers) in a
  `struct _reent` and finds the current one through `_impure_ptr`. The array of these
  structures, one per thread slot, is `s_reent` in `user/src/newlib_reent.cc`, in app memory.
  The kernel read where it is at boot (`reent_seam_read` in `kmain`) and points `_impure_ptr`
  at the incoming thread's entry on every switch.
- **The heap.** `malloc` grows the heap with `_sbrk`, a bump pointer over the unused part of
  the app data window (`_kickos_heap_start` to `_kickos_heap_limit`).
- **The lock.** `getenv` and `malloc` take newlib's lock, which is the kernel mutex Stop 10
  seated. The lock glue asks who is running (`kos_thread_self`) and locks the mutex
  (`kos_mutex_lock`): these are root's first two syscalls.

**Try it:**

```
(gdb) break kickos_root_entry
(gdb) continue
(gdb) x/3a &__kickos_app_init_array_start
0x109c0:	0xd901 <register_fini>	0xded5 <kickos_exception_report_install()>
	0x392d <_GLOBAL__sub_I__ZN9__gnu_cxx9__freeresEv>
(gdb) p _impure_ptr
$1 = (_reent *) 0x200105b8 <s_reent>
(gdb) break kos_mutex_lock
(gdb) continue
Breakpoint 2, kos_mutex_lock (mtx=1) at .../user/src/syscall_stubs.cc:85
(gdb) bt
#0  kos_mutex_lock (mtx=1) at .../user/src/syscall_stubs.cc:85
#1  (anonymous namespace)::libc_lock_acquire () at .../user/src/newlib_lock.cc:34
#2  _findenv_r (reent_ptr=0x200105b8 <s_reent>, name=0x10474 "GLIBCXX_TUNABLES", ...) at .../newlib/libc/stdlib/getenv_r.c:84
#3  getenv (name=<optimized out>) at .../newlib/libc/stdlib/getenv.c:82
#4  _GLOBAL__sub_I__ZN9__gnu_cxx9__freeresEv ()
(gdb) delete
(gdb) break _sbrk
(gdb) continue
Breakpoint 3, _sbrk (incr=incr@entry=1112) at .../user/src/newlib_sbrk.cc:59
(gdb) bt
#0  _sbrk (incr=incr@entry=1112) at .../user/src/newlib_sbrk.cc:59
#1  _sbrk_r (ptr=ptr@entry=0x200105b8 <s_reent>, incr=incr@entry=1112) at .../newlib/libc/reent/sbrkr.c:51
#2  malloc_extend_top (reent_ptr=0x200105b8 <s_reent>, nb=<optimized out>) at .../newlib/libc/stdlib/_mallocr.c:2170
#3  _malloc_r (reent_ptr=0x200105b8 <s_reent>, bytes=<optimized out>) at .../newlib/libc/stdlib/_mallocr.c:2585
#4  _GLOBAL__sub_I__ZN9__gnu_cxx9__freeresEv ()
(gdb) info registers control
control        0x3                 3
```

Root (slot 0) uses `s_reent[0]`. The libc lock is handle 1 in root's table. The libstdc++
pool constructor (`eh_alloc`) asks for its buffer, and the heap grows by 1112 bytes, all of it
unprivileged. The walk through `kos_mutex_lock` into the kernel is [02](02-first-syscall.md)
and [04](04-mutex.md).

**Why, and the contract:** [Book: what's under #include](../book/whats-under-include-libc-and-the-cxx-runtime.md),
[Book: where your RAM goes](../book/where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md),
[Reference: `ctors-run-before-init-entry`](../reference/invariants.md#app-entry--init-seam)

### Stop 16: the init reads the composition and starts the `main` task

**Where:** `system/init/compose/entry.cc`, `kickos_init_entry`; `system/init/compose/walk.cc`,
`Walk::run`, `Walk::boot`, `Walk::start`, `Walk::spawn`; `boards/qemu/composition.yaml`;
`user/src/syscall_stubs.cc`, `kos_task_create`, `kos_thread_create`

After the constructors, root becomes the **init**. `kickos_init_entry` walks a table that the
build generated from the composition (`boards/qemu/composition.yaml`). For a plain application
the table holds one task:

```yaml
tasks:
  - name: main
    entry: kickos_main
    stack: 8192
    priority: 2
    ceiling: 31
    authority: [memory, system, tasks]
```

`Walk::boot` first lowers root to the init's priority (2). `Walk::start` then makes a new task
with `kos_task_create` and gives it its first thread with `kos_thread_create`. Both are
ordinary syscalls; [08](08-spawn-and-caps.md) walks them in full.

```c++
        kos_thread_params p = {};
        p.entry = task_trampoline;
        p.arg = const_cast<kos_table_task*>(&t);
        p.name = name(s.task);
        p.prio = t.priority;
        p.policy = KOS_POLICY_FIFO;
        // ...
        p.authority = t.authority;
        p.task = s.handle;
        kos_thread_t thread = KOS_THREAD_NONE;
        return kos_thread_create(&p, &thread);
```

Inside the kernel, `kos_thread_create` ends in the same `thread_create` and
`arch_context_init` you saw in Stop 8: a fabricated frame, unprivileged, entry
`task_trampoline`. The new thread is ready but does not run yet. It has priority 2, the same as
the init, and both are FIFO, so it waits until the init blocks. The init blocks in
`Walk::wake`, waiting for news about its tasks.

**Try it:**

```
(gdb) break kos_thread_create
(gdb) continue
Breakpoint 1, kos_thread_create (params=0x20023e18, out_thread=0x20023de4) at .../user/src/syscall_stubs.cc:378
(gdb) p *params
$1 = {entry = 0x19f9 <kickos::init::task_trampoline(void*)>, arg = 0x10420 <kickos_table_image+32>,
      name = 0x10458 <kickos_table_image+88> "main", prio = 2 '\002', policy = 0 '\000',
      cap_count = 0 '\000', ..., stack_base = 0x0, stack_size = 0, ..., authority = 81, task = 3}
(gdb) break arch_context_init
(gdb) continue
Breakpoint 2, arch_context_init (ctx=0x20004e90 <kickos::detail::g_instance+1760>,
    entry=0x19f9 <kickos::init::task_trampoline(void*)>, arg=0x10420 <kickos_table_image+32>,
    stack_base=0x20026000, stack_size=8192, privileged=0) at .../arch/arm/armv7m/arch_armv7m.cc:218
(gdb) bt 6
#0  arch_context_init (...) at .../arch/arm/armv7m/arch_armv7m.cc:218
#1  kickos::thread_create (...) at .../kernel/thread/thread.cc:652
#2  kickos::spawn_masked (...) at .../kernel/syscall/syscall_thread.cc:1144
#3  kickos::thread_create_call (...) at .../kernel/syscall/syscall_thread.cc:1168
#4  (anonymous namespace)::syscall_body (nr=7, ...) at .../kernel/syscall/syscall.cc:713
#5  syscall_dispatch (nr=7, ...) at .../kernel/syscall/syscall.cc:400
```

`authority = 81` is `0x51`: the three bits the composition lists, narrower than root's `0xff`.
The stack is left to the kernel (`stack_base = 0x0`): on this board every thread stack is one
power-of-two block from the arena, here `0x20026000`. The backtrace crosses the user/kernel
line: the syscall entered the kernel and the dispatcher called the thread-creation code. This
stop, unlike Stop 14, can read kernel memory, because the core is now running privileged inside
a syscall.

**Why, and the contract:** [Reference: `app-entry-via-init-seam`](../reference/invariants.md#app-entry--init-seam),
[Reference: user/kernel separation](../reference/architecture.md#userkernel-separation)

### Stop 17: the first instruction of `main`

**Where:** `user/src/task_trampoline.cc`, `kickos::init::task_trampoline`;
`system/init/compose/main.cc`, `kickos_main`; `user/include/kickos/app.h`, `kickos_app_main`;
`user/apps/common/hello/main.cc`, `main`

When the init blocks, the scheduler picks the `main` thread, and a PendSV restores its
fabricated frame exactly as in Stop 13. This time the outgoing thread (the init) is saved
first, and the MPU record is already valid: `main`'s region set differs from root's only in
the stack region, so the commit rewrites that one descriptor. The thread starts in
`task_trampoline`, which calls the task's entry from the table, `kickos_main`:

```c++
    void task_trampoline(void* self)
    {
        auto const task = static_cast<kos_self_t const*>(self);
        task->entry.task(task);
        // Never exit(): its handlers and destructors are the image's, which every task shares.
        fflush(stdout);
        fflush(stderr);
        kos_exit(0);
    }
```

```c++
extern "C" void kickos_main(struct kos_table_task const* self)
{
    (void)self;
    exit(kickos_app_main(kickos_init_args.argc, kickos_init_args.argv));
}
```

`kickos_app_main` is the application's `main`. The build compiles every application file with
`-Dmain=kickos_app_main` and force-includes `kickos/app.h`, which declares it `extern "C"`. So
you write an ordinary `int main(int argc, char** argv)`, and the kernel never calls it
directly. When `main` returns, `exit` runs, and because the composition says the system
`ends: main`, the whole system shuts down with that status. `hello` never returns.

**Try it:** first the switch into `main`. The set waiting for the commit names `main`'s stack
at `0x20026000`, where root's named `0x20022000`; the other two regions are the same words.

```
(gdb) break kickos::init::Walk::wake
(gdb) continue
(gdb) break kickos_arch_mpu_commit
(gdb) continue
(gdb) bt
#0  kickos_arch_mpu_commit () at .../arch/arm/common/kickos_arch_mpu_commit_default.cc:28
#1  PendSV_Handler () at .../arch/arm/armv7m/switch.S:220
(gdb) p/x *kickos_arm_mpu_pending()
$1 = {rbar = {0x0, 0x20010000, 0x20026000, 0x0, 0x0, 0x0, 0x0, 0x0},
      rasr = {0x603002b, 0x1303001f, 0x13030019, 0x0, 0x0, 0x0, 0x0, 0x0}}
```

Then `main` itself. `break main` does not work, because no function has that name in the
image.

```
(gdb) break main
Function "main" not defined.
(gdb) break kickos_app_main
(gdb) continue
Breakpoint 1, kickos_app_main () at .../user/apps/common/hello/main.cc:58
58	    kos::print("hello from KickOS userspace!\n");
(gdb) bt
#0  kickos_app_main () at .../user/apps/common/hello/main.cc:58
#1  kickos_main (self=<optimized out>) at .../system/init/compose/main.cc:15
#2  kickos::init::task_trampoline (self=<optimized out>) at .../user/src/task_trampoline.cc:16
#3  kos_panic (msg=<optimized out>) at .../user/src/syscall_stubs.cc:405
(gdb) info registers pc sp psp control
pc             0x190               0x190 <kickos_app_main(int, char**)>
sp             0x20027ff0          0x20027ff0
psp            0x20027ff0          537034736
control        0x3                 3
(gdb) p _impure_ptr
$1 = (_reent *) 0x200106f8 <s_reent+320>
```

This is the end of the walk: `main`, unprivileged (`CONTROL = 3`), on its own stack just below
`0x20028000`, with its own C library state (`s_reent[1]`, one `_reent` further than root's).
Frame #3 is not a panic: it is the return address the kernel planted,
`kickos_user_thread_return`, which gdb names by the function just before it (see the
[README](README.md#reading-with-gdb-or-without-it)).

The next line calls `kos::print`, which ends in a syscall. That is where
[02](02-first-syscall.md) begins.

**Why, and the contract:** [Reference: `app-entry-via-init-seam` and `init-return-is-shutdown`](../reference/invariants.md#app-entry--init-seam)

## Where to next

- [2. The first syscall](02-first-syscall.md): what happens on `kos::print`.
- [8. Spawn and capabilities](08-spawn-and-caps.md): `kos_task_create` and `kos_thread_create`
  from Stop 16, in full.
- [10. The MMU](10-the-mmu.md): the same boot on `qemu-arm64`, where root's regions become an
  address space.

## Exercises

1. **A constructor of your own.** Add a global object with a constructor to
   `user/apps/common/hello/main.cc` (for example one that sets a global `int` to 42). Predict:
   which list holds it, which thread runs it, and at what privilege. Check: break on the
   constructor, then `bt` and `info registers control`. You should see `kickos_root_entry` in
   the backtrace and `control = 3`.
2. **The MPU's view.** At Stop 14, `x/wx 0x20010000` (the app data window) and
   `x/wx 0x20000000` (the kernel's data). Predict which read works. Then do the same two reads
   at Stop 16, inside the syscall, and explain the difference.
3. **Who runs first.** Change the `main` task's `priority` in `boards/qemu/composition.yaml`
   from 2 to 3 and rebuild. Predict whether `kickos_app_main` is now reached before or after
   `Walk::wake`. Check with breakpoints on both.
