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

A note on the two names for the kernel's state: the kernel keeps its own state in one `Kernel`
structure (and its object pools beside it, Stop 6), reached in C++ through `kickos::kernel()`. That function is inline, so gdb
cannot call it. In gdb, use the instance directly. At reset gdb takes the current frame for C, so
tell it the language first:

```
(gdb) set language c++
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
the 48 device interrupt lines. All but one point at one generic entry, `kickos_arm_default_irq`,
which turns an interrupt into a kernel event ([07](07-interrupt-to-a-driver.md)). The exception is
line 10, the dual timer's, which goes to `kickos_mps2_clock_isr` and keeps the clock counting
(Stop 11).

```
    .section .isr_vector, "a"
    .align 8                         /* 256-byte aligned (VTOR requirement) */
    .global g_isr_vector
    .type g_isr_vector, %object
g_isr_vector:
#if defined(__ARM_ARCH_8M_MAIN__)
    KICKOS_CORE_VECTORS_V8M
#else
    KICKOS_CORE_VECTORS_V7M
#endif

    .rept KICKOS_LAYOUT_LINE_DUALTIMER_IRQ
    .word kickos_arm_default_irq
    .endr
    .word kickos_mps2_clock_isr      /* dual timer 1 (clock wrap) */
    .rept KICKOS_MAX_IRQ - KICKOS_LAYOUT_LINE_DUALTIMER_IRQ - 1
    .word kickos_arm_default_irq
    .endr
```

`KICKOS_LAYOUT_LINE_DUALTIMER_IRQ` is 10, the line the chip file `platform/mps2/chip.yaml` gives
the dual timer.

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
There too one line is different: the TIM2 interrupt goes to `kickos_tim2_clock_isr`, which keeps
the clock counting. The table sits at the start of internal flash, `0x08000000`, which
the chip aliases to address 0 at boot.

**Try it:** attach gdb to a QEMU started with `-S`. Nothing has run yet.

```
(gdb) info registers pc sp
pc             0x250               0x250 <Reset_Handler>
sp             0x20400000          0x20400000
(gdb) x/2wx 0
0x0 <g_isr_vector>:	0x20400000	0x00000251
```

The two words at address 0 are exactly SP and PC. The PC word is `0x251`, odd: bit 0 set means
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
Nobody has done that yet, so the reset handler's job is to do it. Until then, no code
may read a global. (The first two lines of the handler read no global: one sets the core's
configuration and control register, `CCR`, the other switches the FPU on; Stop 3 has the second.)

```c++
void Reset_Handler(void)
{
    kickos_armv7m_ccr_init();
    kickos_armv7m_enable_fpu(); // before any code a hard-float ABI might emit FP into
    kickos_ranges_init();
    kickos_crt_tail();
}
```

`kickos_armv7m_ccr_init` makes a divide by zero fault, lets the core make unaligned accesses
(compiled code and the prebuilt C library make them), and stops unprivileged code from pending an
interrupt line through `STIR`.

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

Why tables, and not one `.data` and one `.bss`? Because there is one pair per program in the
image. The kernel's globals live at the bottom of RAM. Each user image has a window of its own
for its globals (and libc's, and its heap): `hello` is two images, the init (image 0) and the
application (image 1), with sections such as `.kickos_image1_data`. The MPU will later give each
unprivileged thread its own image's window and nothing else, so kernel variables must not sit
inside one.

**Try it:** `SystemCoreClock` is a kernel global with an initial value of 25 MHz. Read it before
and after the copy function:

```
(gdb) p SystemCoreClock
$1 = 0
(gdb) x/9wx &__kickos_copy_table_start
0xf1fc:	0x00013834	0x20010000	0x00000150	0x000113b8
0xf20c:	0x20018000	0x00000148	0x0000f238	0x20000000
0xf21c:	0x00000018
(gdb) x/6wx &__kickos_zero_table_start
0xf220:	0x20010150	0x00007eb0	0x20018148	0x000002b8
0xf230:	0x20000020	0x000030f0
(gdb) break kickos_ranges_init
(gdb) continue
(gdb) finish
(gdb) p SystemCoreClock
$2 = 25000000
```

Read the tables. Copy `0x150` bytes from flash `0x13834` to `0x20010000` (the application's data),
`0x148` bytes from `0x113b8` to `0x20018000` (the init's data), and `0x18` bytes from `0xf238` to
`0x20000000` (the kernel's data). Zero `0x7eb0` bytes at `0x20010150` and `0x2b8` bytes at
`0x20018148`: each covers the rest of its image's window, its bss and its heap. Zero `0x30f0`
bytes at `0x20000020`, the kernel's bss. After the copy and zero loops, the global holds its value.

The frame gdb prints on a stop at low addresses can name the wrong function, here a C library
`_malloc_r` line: debug information of code the link discarded still claims addresses from 0
up. `info symbol $pc` reads the symbol table and names the right one.

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
any constructor runs. It does so right after the `CCR` set-up, before the copy loops of Stop 2.

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

**f411disco:** `Reset_Handler` in `arch/arm/chip/stm32f411/chip_stm32f411.cc` has the same
order, but spells the constructor loop out instead of calling `kickos_crt_tail`. The result is
the same.

**Try it:**

```
(gdb) x/wx 0xE000ED88
0xe000ed88:	0x00000000
(gdb) p &__init_array_start
$1 = (void (*(*)[])(void)) 0xf1ec
(gdb) p &__init_array_end
$2 = (void (*(*)[])(void)) 0xf1ec
(gdb) break arch_init
Breakpoint 1 at 0x1b4: file .../arch/arm/chip/mps2/chip_mps2.cc, line 70.
(gdb) continue
Breakpoint 1, 0x000001b4 in _malloc_r (reent_ptr=0x0 <g_isr_vector>, bytes=<optimized out>) at .../newlib/libc/stdlib/_mallocr.c:2161
(gdb) info symbol $pc
arch_init in section .text
(gdb) x/wx 0xE000ED88
0xe000ed88:	0x00f00000
```

`0xE000ED88` is `CPACR`; `0x00f00000` is full access to CP10 and CP11, the FPU. The two
`__init_array` bounds are equal: zero kernel constructors. The stop is in `arch_init`, which
`info symbol` confirms; the `_malloc_r` frame is the discarded debug information of Stop 2.

**Why, and the contract:** [Book: what's under #include](../book/whats-under-include-libc-and-the-cxx-runtime.md)
(global constructor order on a freestanding OS),
[Reference: `ctors-run-before-their-image-entry`](../reference/invariants.md#app-entry--init-seam)

### Stop 4: `arch_init` brings up the chip: clocks and core priorities

**Where:** `arch/arm/chip/mps2/chip_mps2.cc`, `arch_init`, `dualtimer_clock_init`;
`arch/arm/armv7m/arch_armv7m.cc`, `kickos_armv7m_init`; f411: `arch/arm/chip/stm32f411/chip_stm32f411.cc`, `arch_init`,
`clock_init`, `tim2_clock_init`, `usart2_init`

`arch_init` is the chip's one bring-up function. The kernel calls it once, before `kmain`, and
expects the hardware to be ready afterwards: the CPU at its final speed, the console able to
print, the time base counting.

On QEMU there is little to do. The emulated core has no PLL to configure, so `SystemCoreClock`
is a constant 25 MHz, the board's system clock. The console goes through semihosting, which needs
no set-up. `dualtimer_clock_init` starts the clock: Timer 1 of the board's dual timer counts down
from `0xFFFFFFFF` at that same 25 MHz, and its wrap interrupt, line 10, is unmasked (Stop 11). What
remains is the part every ARMv7-M chip shares, `kickos_armv7m_init`:

```c++
void kickos_armv7m_init(void)
{
    // ...
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
(gdb) x/wx 0xE000E100
0xe000e100:	0x00000400
```

`SHPR2` holds SVCall's priority in its top byte (`0xe0`); `SHPR3` holds SysTick (`0xe0`) and
PendSV (`0xf0`). `NVIC_ISER0` has bit 10 set: the dual timer's line is the one device interrupt
enabled so far.

**Why, and the contract:** [Book: interrupts and traps](../book/interrupts-and-traps.md)
(masking), [Reference: critical section, timer, clock, NVIC](../reference/porting.md#critical-section-timer-clock-nvic)

### Stop 5: `kmain` starts, and the banner goes out through the early console

**Where:** `kernel/init/kmain.cc`, `kmain`, `kbanner`; `kernel/init/console.cc`, `kputs`,
`console_write_line_sync`; `kernel/init/console_tx.cc`, `console_tx_insert_line`;
`arch/common/semihost_console.cc`, `arch_console_write_sync`

`kmain` is the kernel's `main`. It runs on the boot stack (MSP), privileged. On QEMU the one
interrupt source enabled so far is the dual timer's wrap, which comes once every 172 s. Its first
lines publish `argc`/`argv` for the init (into app-side storage, because the init will read them
unprivileged), then print the banner, then build the kernel's objects:

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
ring that is drained in the background. The ring is not armed yet, so `console_tx_insert_line`
writes the line directly with `console_write_line_owned`: each byte goes out through the chip's
`arch_console_write_sync` in its own short masked span (`console_retry::write_masked`), with a
`\r` sent before each `\n`. Interrupts stay open between two bytes. A panic writes its lines
through `write_line_polled` instead, which takes no lock, so the kernel can always print.

On QEMU, `arch_console_write_sync` hands each byte to the host with the semihosting call
`SYS_WRITEC`. **f411disco:** it writes the USART2 data register directly, waiting for the
transmit-empty flag.

**Try it:**

```
(gdb) break arch_console_write_sync
(gdb) continue
(gdb) bt
#0  arch_console_write_sync (buf=0x203fff5f "\r", <incomplete sequence \356>, n=1) at .../arch/common/semihost_console.cc:36
#1  0x00003802 in (anonymous namespace)::line_owned_put (c=<optimized out>, c@entry=13 '\r') at .../kernel/init/console.cc:534
#2  0x00003cd8 in operator() (__closure=<optimized out>, c=13 '\r') at .../kernel/init/console.cc:568
#3  operator() (__closure=<synthetic pointer>, c=13 '\r') at .../arch/include/kickos/arch/console_retry.h:41
#4  kickos::console_retry::write<...> (buf=<optimized out>, n=1, put=...) at .../arch/include/kickos/arch/console_retry.h:21
#5  kickos::console_retry::write_masked<...> (buf=<optimized out>, n=1, put=...) at .../arch/include/kickos/arch/console_retry.h:39
#6  console_write_line_owned (buf=<optimized out>, n=1) at .../kernel/init/console.cc:567
#7  0x00003a52 in kickos::console_emit (buf=buf@entry=0xdb39 "\n", n=1, flags=flags@entry=0 '\000') at .../kernel/init/console.cc:620
#8  0x00003d0a in kickos::kconsole_write_impl (buf=buf@entry=0xdb39 "\n", n=<optimized out>, flags=0 '\000') at .../kernel/init/console.cc:679
#9  0x00003d80 in kickos::kputs (s=s@entry=0xdb39 "\n") at .../kernel/init/console.cc:738
#10 0x000052da in kickos::(anonymous namespace)::kbanner () at .../kernel/init/kmain.cc:103
#11 kickos::kmain (argc=0, argv=0x0 <g_isr_vector>) at .../kernel/init/kmain.cc:263
#12 0x00000286 in _malloc_r (...) at .../newlib/libc/stdlib/_mallocr.c:2252
#13 <signal handler called>
(gdb) p buf[0]@n
$1 = "\r"
```

The first byte the kernel ever sends is the `\r` in front of the banner's leading newline. Read
the stack from the bottom up: it is the whole boot path so far. Frame #12 is `Reset_Handler`
(`info symbol 0x286` says so), named wrongly for the reason Stop 2 gives, and gdb's unwinding
ends below it.

**Why, and the contract:** [Reference: console](../reference/console.md)

### Stop 6: the kernel's objects are already there; `kmain` puts them in their start state

**Where:** `kernel/init/instance.cc`, `g_instance`; `kernel/include/kickos/instance.h`,
`Kernel`, `kernel()`; `include/kickos/pools.h`, `KernelPools`; `kernel/include/kickos/pool_storage.h`,
`KICKOS_SYSTEM_POOLS`; `kernel/sched/sched.cc`, `sched::init`; `kernel/domain/domain.cc`,
`domain_init`; `kernel/task/task.cc`, `task_init`; `kernel/irq/irq.cc`, `irq_init`;
`kernel/init/console_tx.cc`, `console_buffer_init`

The kernel allocates nothing at run time for its own bookkeeping. Its state is in two places,
both declared `constinit`. One `Kernel` structure, `g_instance`, holds what every system has:
the ready queues, the running and idle seats, the idle thread itself, the sleep queues and the
interrupt table. The **pools**, the objects whose number each system chooses (the thread slots
and their kernel stacks, the tasks, the memory domains, the word records), are not in the kernel
archives at all. The build generates their storage for each system from its composition
(`KICKOS_SYSTEM_POOLS`), and the kernel reads each pool's base and count from one constant
descriptor, `kickos_kernel_pools`. `constinit` makes the compiler prove that an object needs no
constructor, so both live in `.bss` and are ready the moment Stop 2 zeroed it. That is why Stop 3
could run zero kernel constructors.

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
(gdb) set language c++
(gdb) p &kickos::detail::g_instance
$1 = (kickos::InstanceLocal<kickos::Kernel> *) 0x200004e8 <kickos::detail::g_instance>
(gdb) p sizeof(kickos::Kernel)
$2 = 1568
(gdb) p kickos::kickos_kernel_pools.thread_slots
$3 = 5
(gdb) info symbol kickos::kickos_kernel_pools.threads
kickos::(anonymous namespace)::system_pools + 6192 in section .bss
(gdb) break kickos::domain_init
(gdb) continue
(gdb) finish
(gdb) p kickos::kickos_kernel_pools.domains[0].privileged
$4 = true
(gdb) p/x kickos::kickos_kernel_pools.domains[0].regions[0]
$5 = {base = 0x20018400, size = 0x3e3c00, attr = 0x3}
```

The `Kernel` structure is about 1.5 KiB. The pools, `system_pools`, are in `.bss` beside it,
sized for this system: 5 thread slots, and with them the per-thread kernel stacks the banner
reports (`kstack 1008 B x 5`). The kernel domain covers `0x20018400` to the end of RAM, read and
write (`attr = 0x3`, see `ARCH_MPU_R` and `ARCH_MPU_W` in `arch.h`).

**Why, and the contract:** [Reference: scheduler](../reference/architecture.md#scheduler-the-core-constraint),
[Reference: memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 7: the arena hands out the first two stacks

**Where:** `kernel/init/kmain.cc`, `boot_stack_alloc`; `arch/common/arch_ram_common.cc`,
`arch_ram_alloc`, `arch_ram_base`, `arch_ram_size`; `arch/common/ram_arena.h`, `ram_arena_alloc`

The RAM above the kernel's static data and the images' windows is the **arena**: a block the
kernel hands out with a bump allocator. Nothing is ever given back. The first two pieces are
stacks: one for the **idle** thread (the thread that runs when nothing else can) and one for
**root** (the first user thread). Idle comes first, and the order is fixed: the linker script
replays exactly these two allocations to check at link time that they fit.

```c++
        uintptr_t const cur = base + a.used;
        uintptr_t at = (cur + (ralign - 1)) & mask;
        // ...
        size_t const off = static_cast<size_t>(at - base);
        if (at < cur or off > total or rsz > total - off)
        {
            return nullptr;
        }
        size_t const run = static_cast<size_t>(at - cur);
        if (run > a.gap_len)
        {
            a.gap_off = a.used;
            a.gap_len = run;
        }
        a.used = off + rsz;
        return reinterpret_cast<void*>(at);
```

Notice the alignment. A PMSAv7 MPU region must be a power of two in size and start at a
multiple of its size. Root's stack will be one MPU region, so its 8 KiB block must start on an
8 KiB boundary. The allocator rounds up, and keeps the largest run-up it skipped as one free
range (`gap_off`, `gap_len`), which a later block that fits takes first.

**Try it:**

```
(gdb) p/x arch_ram_base()
$1 = 0x20018400
(gdb) p/x arch_ram_size()
$2 = 0x3e3c00
(gdb) break arch_ram_alloc
(gdb) continue
Breakpoint 1, arch_ram_alloc (size=512) at .../arch/common/arch_ram_common.cc:89
(gdb) finish
Value returned is $3 = (void *) 0x20018400
(gdb) continue
Breakpoint 1, arch_ram_alloc (size=8192) at .../arch/common/arch_ram_common.cc:89
(gdb) finish
Value returned is $4 = (void *) 0x2001a000
```

Idle's 512 bytes take the start of the arena. Root's 8 KiB cannot start at `0x20018600`; the
next 8 KiB boundary is `0x2001a000`. The 6.5 KiB in between is the price of the MPU's rule,
kept for a later block small enough to fit there.

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

    for (int i = 0; i < 8; i++)
    {
        *(--sp) = 0; // r11..r4
    }

    ctx->sp = reinterpret_cast<uint32_t>(sp);
    ctx->exc_return = 0xFFFFFFFDu; // thread mode, PSP, non-FP frame
    // ...
    ctx->npriv = npriv;
```

The top eight words are what the hardware pushes on an exception. The lower eight are what
`PendSV_Handler` pushes: `r4` to `r11`. The `EXC_RETURN` code that tells the core how to return
is kept in the context itself, `ctx->exc_return`, never on a stack the thread can write. `npriv`
is the thread's privilege. **This is where root becomes unprivileged:** `kmain` sets
`root_attr.privileged = false`, so `npriv` is 1 from the first instruction. There is no later
moment where root is demoted. (A thread inside a syscall does run kernel code privileged, for the
length of the call; [02](02-first-syscall.md) shows how. Its resting privilege, the one
`resting_npriv` records, never changes.) Idle is the only privileged thread in the system.

Before the frame, `thread_create` carves the thread's C library state off the base of its stack
block, so the stack the frame is built on starts above it. Stop 15 comes back to it.

**Try it:** `arch_context_init` sits at a low address, so gdb names a wrong frame where it stops
(see Stop 2). Read the arguments from the registers instead: `r0` is `ctx`, `r1` the entry,
`r3` the stack base, and the stack size and privilege flag are the two words on the stack.

```
(gdb) break arch_context_init
(gdb) continue
(gdb) info symbol $r1
kickos::(anonymous namespace)::idle_entry(void*) + 1 in section .text
(gdb) p/x $r3
$1 = 0x20018400
(gdb) p *(unsigned*)$sp
$2 = 512
(gdb) p *((int*)$sp+1)
$3 = 1
(gdb) continue
(gdb) p/x $r0
$4 = 0x20002370
(gdb) info symbol $r1
kickos_root_entry + 1 in section .kickos_image0_text
(gdb) p/x $r3
$5 = 0x2001a180
(gdb) p *(unsigned*)$sp
$6 = 7808
(gdb) p *((int*)$sp+1)
$7 = 0
(gdb) break kickos::sched::start
(gdb) continue
(gdb) set $t = &kickos::kickos_kernel_pools.threads[0]
(gdb) p/x $t->ctx
$8 = {sp = 0x2001bfc0, npriv = 0x1, resting_npriv = 0x1, stack_lo = 0x2001a180, stack_hi = 0x2001c000, kernel_sp = 0x20000f30, exc_return = 0xfffffffd}
(gdb) x/16wx $t->ctx.sp
0x2001bfc0:	0x00000000	0x00000000	0x00000000	0x00000000
0x2001bfd0:	0x00000000	0x00000000	0x00000000	0x00000000
0x2001bfe0:	0x00000000	0x00000000	0x00000000	0x00000000
0x2001bff0:	0x00000000	0x00010963	0x00010a98	0x01000000
```

Idle gets the 512 bytes at the arena's start, privileged (1). Root's block starts at
`0x2001a000`, but its stack starts `0x180` bytes above it, past its C library state, so it is
7808 bytes, unprivileged (0). Read the dump from low to high: eight zeros (`r4`-`r11`), then the
hardware frame: `r0`-`r3` and `r12` all 0, LR `0x10963`, PC `0x10a98`, xPSR `0x01000000`. PC is
`kickos_root_entry`. LR is `kickos_user_thread_return` (with the Thumb bit): an unprivileged
thread whose entry function returns lands there, and it exits through a syscall, since such a
thread cannot do the kernel's exit work itself. `exc_return` waits in the context. Root lives in
thread slot 0. `kernel_sp` is the top of root's own kernel stack, where its syscalls will run
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
2. its image's data window, read and write: the init's (image 0) data, bss and heap;
3. root's own stack block, read and write, its C library state included.

Nothing else. In particular the kernel's `.data` and `.bss` (at `0x20000000`) and every other
thread's stack are outside all three. `MpuSet` also keeps the regions already encoded in the
MPU's register format (`image_`), so the switch writes words and computes nothing.

**Try it:** at `kickos::sched::start`, as in Stop 8:

```
(gdb) p/x $t->mpu
$1 = {... regions_ = {{base = 0x0, size = 0x400000, attr = 0x5},
      {base = 0x20018000, size = 0x400, attr = 0x3},
      {base = 0x2001a000, size = 0x2000, attr = 0x3}, ...}, count_ = 0x3, ...
      image_ = {rbar = {0x0, 0x20018000, 0x2001a000, 0x0, ...},
                rasr = {0x603002b, 0x13030013, 0x13030019, 0x0, ...}}}
```

`attr = 0x5` is read and execute, `0x3` read and write. In the encoded `rasr` words you can
see the size field (bits 5:1: `0x15` for 4 MiB, `0x9` for 1 KiB, `0xc` for 8 KiB) and, on the
two data regions, bit 28: execute-never.

**Why, and the contract:** [Book: protection follows the CPU, not the scheduler's intent](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Reference: memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 10: root receives its capability table and its authority

**Where:** `kernel/init/kmain.cc`, `kmain`; `kernel/syscall/cap.cc`, `cap_slab_init`,
`cap_slab_attach`, `cap_seat_authority`

A user thread can name a kernel object only through a **capability**: an entry in the thread's own
capability table that names the object and the rights the thread has over it. User code holds a
**handle**, the number that picks the entry. Root gets a table with room for
the init's width in entries (8 here, `cap_root_width`, which the composition sets) out of a shared
slab (`cap_slab_attach`).

One thing is seated before root runs: **the authority word** (`cap_seat_authority`), a set of
bits for acts that name no object: shutting down the system, creating tasks, reserving memory.
Root gets all of them (`CAP_AUTH_ALL`). The init will hand each task only the bits its
composition lists. The comment in `kmain` gives the order: after `thread_create`, which zeroes
the TCB, and before `sched::start`, so the word is in place before root's first instruction.

The C library's locks are not seated anywhere, and the kernel holds nothing for them. Each is a
`struct __lock`, a mutex word and a depth, in the static data of the image that owns the heap,
and the kernel hears of one only when a thread has to wait for it (Stop 15 meets the first one).

**Try it:**

```
(gdb) break kickos::cap_seat_authority
(gdb) continue
Breakpoint 1, kickos::cap_seat_authority (t=t@entry=0x20002370 <kickos::(anonymous namespace)::system_pools+6192>,
    auth=auth@entry=1023) at .../kernel/syscall/cap.cc:1305
(gdb) set $t = t
(gdb) p/x $t->authority
$1 = 0x0
(gdb) finish
(gdb) p/x $t->authority
$2 = 0x3ff
```

Ten authority bits, all set (`KOS_AUTH_ALL` in `system/include/kickos/sys/cap_rights.h`).

**Why, and the contract:** [Book: Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md),
[Book: authority from possession or from identity](../book/authority-from-possession-or-from-identity.md),
[Reference: object model, capabilities and IPC](../reference/architecture.md#object-model-capabilities--ipc)

### Stop 11: the time base and the timer at start: nothing to wait for yet

**Where:** `kernel/sched/sched.cc`, `sched::start`; `kernel/time/time.cc`, `ktime_rearm`;
`arch/arm/common/arch_arm_common.cc`, `arch_timer_arm`, `arch_timer_disarm`;
`arch/arm/chip/mps2/chip_mps2.cc`, `arch_clock_now`, `kickos_mps2_clock_isr`

KickOS is **tickless**: there is no periodic timer interrupt. There are two separate pieces of
hardware behind time:

- a **clock**, `arch_clock_now`, which returns nanoseconds since boot and never goes back;
- a **one-shot timer**, `arch_timer_arm(deadline)`, which interrupts once at a deadline. On
  every Cortex-M chip this is SysTick (`arch/arm/common/arch_arm_common.cc`).

Every time the scheduler picks a thread, `ktime_rearm` computes the nearest deadline: the first
sleeper, or the running thread's time slice. If there is none, it disarms the timer. At
`sched::start` nothing sleeps and root is FIFO (no slice), so the timer is switched off. The
first arm happens when the first thread sleeps ([03](03-sleep.md)).

The clock is a per-chip choice. On QEMU it is the dual timer Stop 4 started: a 32-bit count at
25 MHz, extended to 64 bits in software (`arch_wrap_extend`), so the clock moves in 40 ns steps.
Its wrap interrupt, the line Stop 1 gave `kickos_mps2_clock_isr`, notices each wrap.
[03](03-sleep.md) walks it. **f411disco:** the clock is TIM2, a 32-bit counter at the core
frequency, extended to 64 bits the same way; the TIM2 interrupt from Stop 1 notices each wrap.

**Try it:**

```
(gdb) break arch_timer_disarm
(gdb) break arch_timer_arm
(gdb) continue
Breakpoint 1, arch_timer_disarm () at .../arch/arm/common/arch_arm_common.cc:195
(gdb) bt 3
#0  arch_timer_disarm () at .../arch/arm/common/arch_arm_common.cc:195
#1  0x0000c606 in kickos::sched::start () at .../kernel/include/kickos/held.h:25
#2  0x000054fc in kickos::kmain (argc=<optimized out>, argv=<optimized out>) at .../kernel/init/kmain.cc:442
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
            struct arch_aspace* const rspace = aspace_activate_for(first);
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
r1             0x20002370          536879984
msp            0x203fff90          541065104
psp            0x0                 0
control        0x0                 0
basepri        0x20                32
(gdb) x/wx 0xE000ED94
0xe000ed94:	0x00000000
(gdb) stepi 11
(gdb) x/i $pc
=> 0xc610 <_ZN6kickos5sched5startEv+56>:	msr	BASEPRI, r6
(gdb) x/wx 0xE000ED04
0xe000ed04:	0x1000e800
```

`r1` is root's context (`0x20002370`, the `ctx` you saw in Stop 8). `PSP` is still 0: no thread
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
    cpsid   i
    ldr     r1, =g_arch_next
    ldr     r1, [r1]                /* incoming ctx* */
    str     r1, [r3]                /* g_arch_current = incoming */
    ldr     r0, [r1]                /* incoming->sp */
    ldr     lr, [r1, #F_CTX_EXC_RETURN]
    ldmia   r0!, {r4-r11}
    // ...
    msr     psp, r0
    // ...
    ldr     r2, [r1, #4]            /* incoming->npriv */
    mrs     r3, control
    bfi     r3, r2, #0, #1          /* CONTROL.nPRIV = incoming npriv */
    msr     control, r3
    isb
    // ...
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    ldr     r0, =g_arch_current
    ldr     r0, [r0]
    bl      kickos_switch_landed
    pop     {r0, lr}
    SWITCH_IN_FRAME
    clrex                           /* the incoming thread's cut STREX fails (DDI0403E A3.4.4) */
    cpsie   i                       /* pair consumed; never resume a thread masked */
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */
```

It takes `EXC_RETURN` from the context, pops the eight words Stop 8 fabricated, points `PSP` at
the hardware frame, and sets `CONTROL.nPRIV` to root's 1. Then it programs the MPU for the
incoming thread, and `kickos_switch_landed` seats the incoming thread's C library state
(Stop 15). The order is the
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
    // HFNMIENA keeps the fixed rows in force inside HardFault and NMI.
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_HFNMIENA | MPU_CTRL_PRIVDEFENA;
```

`PRIVDEFENA` keeps the default memory map for privileged code, so the kernel still reaches
everything; unprivileged code reaches only the programmed regions. `HFNMIENA` keeps the MPU on
inside the HardFault and NMI handlers too. **This is the instant the
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
Breakpoint 1, PendSV_Handler () at .../arch/arm/armv7m/switch.S:163
(gdb) info registers xpsr msp psp control lr
xpsr           0x6100000e          1627389966
msp            0x203fff70          541065072
psp            0x0                 0
control        0x0                 0
lr             0xfffffff9          -7
(gdb) p g_arch_current
$1 = {v_ = {_M_b = {_M_p = 0x0 <g_isr_vector>}}}
(gdb) continue
Breakpoint 2, kickos_arm_mpu_program (img=0x20002458 <kickos::(anonymous namespace)::system_pools+6424>) at
    .../arch/arm/common/arch_arm_mpu_pmsav7.cc:84
(gdb) x/wx 0xE000ED94
0xe000ed94:	0x00000000
(gdb) finish
(gdb) x/wx 0xE000ED94
0xe000ed94:	0x00000007
```

The low byte of `xpsr` is the active exception: `0xe`, PendSV. `lr = 0xfffffff9` says the
exception interrupted thread mode running on `MSP`: the boot code in `sched::start`.
`MPU_CTRL` goes from 0 to 7: `ENABLE` (bit 0), `HFNMIENA` (bit 1) and `PRIVDEFENA` (bit 2).

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
Breakpoint 1, kickos_root_entry () at .../user/src/root_entry.cc:17
(gdb) info registers pc sp msp psp control basepri
pc             0x10a98             0x10a98 <kickos_root_entry(void*)>
sp             0x2001c000          0x2001c000
msp            0x203fff70          541065072
psp            0x2001c000          536985600
control        0x3                 3
basepri        0x0                 0
(gdb) p kickos::detail::g_instance.per_instance[0].live
Cannot access memory at address 0x200005f4
```

`CONTROL = 3`: bit 0 (`nPRIV`) says unprivileged, bit 1 (`SPSEL`) says the thread stack, `PSP`.
`PSP` is `0x2001c000`, the top of root's stack: the hardware frame has been popped. And gdb
itself can no longer read the kernel's memory. QEMU checks the debugger's reads against the MPU
as the core sees it now, and the kernel's `.bss` is in none of root's three regions. The
isolation is real, and you just watched it refuse.

**Why, and the contract:** [Book: privilege is three axes, not one bit](../book/privilege-is-three-axes-not-one-bit.md),
[Reference: user/kernel separation](../reference/architecture.md#userkernel-separation)

### Stop 15: the C and C++ runtime starts in user mode

**Where:** `user/src/root_entry.cc`, `kickos_root_entry`; `user/src/image_start.cc`,
`kickos_image_start_once`, `image_start`; `arch/arm/common/cortexm.ld.h`,
`.kickos_app_init_array`; `user/src/newlib_reent.cc`, `kickos_self_lock_id`;
`kernel/thread/reent.cc`, `reent_seam_read`, `reent_cell_seat`, `reent_prime`, `reent_seat`;
`user/src/newlib_sbrk.cc`, `_sbrk`; `user/src/newlib_lock.cc`

Root now does what a normal C runtime does before `main`: it runs the constructors, the ones
Stop 3 kept out of the kernel's list. Each image runs its own: `kickos_root_entry` runs the
init's image's, and the first task the init starts runs the app's image's, in
`kickos_task_enter`, before the task's own entry (Stop 17). Both call `kickos_image_start_once`,
which takes the image's C library lock, and its `image_start` walks the image's list:

```c++
    void image_start()
    {
        // ...
        for (void (**fn)() = __kickos_app_init_array_start; fn != __kickos_app_init_array_end;
             fn++)
        {
            (*fn)();
        }
    }
```

Then `kickos_root_entry` calls `kickos_init_entry`. On a board with an MMU, `kickos_task_start`
runs `kickos_image_start_once` instead, once for each space. See [10](10-the-mmu.md).

They run unprivileged, one after another, with the kernel fully alive: the init's on root's
stack at root's priority 31, the app's on its first task's stack at that task's priority. So a
constructor may make a syscall. In `hello` the init's image has none, and the app's image has
one, newlib's `register_fini`.

What the C library needs is already in place:

- **The C library state.** newlib keeps per-thread state (`errno`, stdio buffers) in a
  `struct _reent` and finds the current one through `_impure_ptr`, a word in each image's data.
  Each thread's state sits at the base of its own stack block. When the kernel creates a thread
  it copies the image's pristine state there (`reent_prime`) and writes a small cell at its end
  with the state's address and the thread's lock id (`reent_cell_seat`). Each image's figures for
  this were read at boot (`reent_seam_read` in `kmain`). On every switch, where the switch lands,
  `reent_seat` points the incoming thread's `_impure_ptr` at its own state.
- **The heap.** An image that calls `malloc` grows its heap with `_sbrk`, a bump pointer over the
  unused part of the image's data window (`_kickos_heap_start` to `_kickos_heap_limit`).
  `hello` calls none.
- **The locks.** `kickos_image_start_once` takes the image's `__lock___sfp_recursive_mutex`, one
  of newlib's locks, each a mutex word and a depth in the image's static data (Stop 10). The
  lock glue reads who is running (`kickos_self_lock_id`, which finds the cell through
  `_impure_ptr`) and locks the word (`kos_mutex_lock`) with one compare-and-swap in user mode:
  nobody else holds it, so none of this enters the kernel.

**Try it:**

```
(gdb) break kickos_root_entry
(gdb) continue
(gdb) p &__kickos_image0_app_init_array_start
$1 = (<text variable, no debug info> *) 0x113b8
(gdb) p &__kickos_image0_app_init_array_end
$2 = (<text variable, no debug info> *) 0x113b8
(gdb) x/a &__kickos_image1_app_init_array_start
0x13830:	0x12359 <register_fini>
(gdb) p &__kickos_image1_app_init_array_end
$3 = (<data variable, no debug info> *) 0x13834
(gdb) p _impure_ptr
$4 = (_reent *) 0x2001a000
(gdb) x/2wx 0x2001a178
0x2001a178:	0x2001a000	0x00000001
(gdb) break kos_mutex_lock
(gdb) continue
Breakpoint 2.1, kos_mutex_lock (m=0x20018164 <__lock___sfp_recursive_mutex>, until=...) at .../user/src/mutex.cc:41
(gdb) bt
#0  kos_mutex_lock (m=0x20018164 <__lock___sfp_recursive_mutex>, until=...) at .../user/src/mutex.cc:41
#1  0x00010c20 in (anonymous namespace)::libc_lock_take (lock=0x20018164 <__lock___sfp_recursive_mutex>, until=...) at .../user/src/newlib_lock.cc:65
#2  0x00010aec in kickos_image_start_once () at .../user/src/image_start.cc:125
#3  0x00010a9e in kickos_root_entry () at .../user/src/root_entry.cc:17
#4  0x00010962 in kos_panic (msg=<optimized out>) at .../user/src/syscall_stubs.cc:352
(gdb) p/x *m
$5 = {word = 0x0}
(gdb) tbreak *0x10c20
(gdb) continue
(gdb) x/2wx 0x20018164
0x20018164 <__lock___sfp_recursive_mutex>:	0x00000001	0x00000000
```

The init's list is empty (its two bounds are equal); the app's holds one entry. Root's state is
the base of its stack block, `0x2001a000`, and the cell at the end of that state names it and
root's lock id, 1. The lock `kos_mutex_lock` is handed is a lock in the init image's static
data, not a slot of root's table. The word was 0, free, and after the compare-and-swap it holds
that 1: root holds the lock, and the kernel never saw it. (`libc_lock_take` sets the depth just
after.) Frame #4 is not a caller: it is the return address the kernel planted,
`kickos_user_thread_return`, which gdb names by the function just before it (see the
[README](README.md#reading-with-gdb-or-without-it)). The walk through a contended
`kos_mutex_lock` into the kernel is [04](04-mutex.md).

**Why, and the contract:** [Book: what's under #include](../book/whats-under-include-libc-and-the-cxx-runtime.md),
[Book: where your RAM goes](../book/where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md),
[Reference: `ctors-run-before-their-image-entry`](../reference/invariants.md#app-entry--init-seam)

### Stop 16: the init reads the composition and starts the `main` task

**Where:** `system/init/compose/entry.cc`, `kickos_init_entry`; `system/init/compose/walk.cc`,
`Walk::run`, `Walk::boot`, `Walk::start`, `Walk::create`; `boards/qemu/composition.yaml`;
`user/src/syscall_stubs.cc`, `kos_task_create`, `kos_watch`

After the constructors, root becomes the **init**. `kickos_init_entry` walks a table that the
build generated from the composition (`boards/qemu/composition.yaml`). For a plain application
the table holds one task:

```yaml
tasks:
  - name: main
    program: main
    stack: 8192
    priority: 2
    ceiling: 31
    authority: [memory, system, tasks]
```

`Walk::boot` first lowers root to the init's priority (2) and finds the image's program with
`kos_program_find`. `Walk::start` then calls `Walk::create`, which asks the kernel to build the
task and start it with `kos_task_create`. Then it sets a watch on the task with
`kos_watch`. Both are ordinary syscalls; [08](08-spawn-and-caps.md) walks them in full. (A create
answers `-KOS_EAGAIN` while an earlier incarnation of the task is still dying; the init then
watches for the instance to become startable and starts it when the watch reports.)

```c++
        kos_task_params p = {};
        p.instance = name(s.task);
        p.cap_count = static_cast<uint8_t>(t.cap_grant_count);
        // ...
        int rc = kos_task_create(program, &p, &s.handle);
```

The init names the instance and nothing more about the thread. The kernel finds that
instance's declaration in the image, takes its priority and authority from it, builds the task,
and creates the entry thread itself. Inside the kernel, the thread goes through the same
`thread_create` and `arch_context_init` you saw in Stop 8: a fabricated frame, unprivileged,
entry `kickos_program_entry`, the start a program's image header names. The new thread is ready
but does not run yet. It has priority 2, the
same as the init, and both are FIFO, so it waits until the init blocks. The init blocks in
`Walk::wake`, waiting for news about its tasks.

**Try it:** `break kos_task_create` and `continue` stop on the init's call. `break
arch_context_init` and `continue` then stop in the kernel, inside the syscall:

```
(gdb) info symbol $r1
kickos_program_entry + 1 in section .kickos_image1_text
(gdb) p/x $r2
$1 = 0x2001dfe0
(gdb) bt
...
#2  0x000019dc in kickos::thread_create (t=0x200024e8 <...>, entry=0x11c69 <kickos_program_entry(void*)>, arg=0x2001dfe0, stack_base=0x2001c000, stack_size=8192, attr=...) at .../kernel/thread/thread.cc:679
#3  0x0000934e in kickos::spawn_locked (...) at .../kernel/syscall/syscall_thread.cc:1288
#4  0x000099a2 in kickos::(anonymous namespace)::task_create_locked (...) at .../kernel/syscall/syscall_thread.cc:2257
#5  kickos::task_create_call (prog=prog@entry=2, pu=pu@entry=536985036, out=out@entry=536985260) at .../kernel/syscall/syscall_thread.cc:2308
#6  0x00006e3a in (anonymous namespace)::syscall_body (...) at .../kernel/syscall/syscall.cc:629
#7  syscall_dispatch (...) at .../kernel/syscall/syscall.cc:431
#8  0x00000e0e in svc_trampoline () at .../arch/arm/armv7m/switch.S:690
```

The entry is `kickos_program_entry` and the argument is the `kos_task_args` block at the top of
the new thread's stack block (`0x2001c000`, 8 KiB). Frames #0 and #1, cut here, carry the wrong
names Stop 2 explains. The backtrace crosses the user/kernel line: the syscall entered the kernel and
the dispatcher called `task_create_call`, which called the thread-creation code. This stop,
unlike Stop 14, can read kernel memory, because the core is now running privileged inside a
syscall.

The authority of the thread is the three bits the composition lists (`0x51`), narrower than
root's `0x3ff`. The stack is one power-of-two block from the arena on this board.

**Why, and the contract:** [Reference: `app-entry-via-init-seam`](../reference/invariants.md#app-entry--init-seam),
[Reference: user/kernel separation](../reference/architecture.md#userkernel-separation)

### Stop 17: the first instruction of `main`

**Where:** `user/src/program_entry.cc`, `kickos_program_entry`, `kickos_program_run`;
`user/src/task_enter.cc`, `kickos_task_enter`; `user/apps/common/hello/main.cc`, `main`

When the init blocks, the scheduler picks the `main` thread, and a PendSV restores its
fabricated frame exactly as in Stop 13. This time the outgoing thread (the init) is saved
first, and the MPU record is already valid. `main` runs the application's image, not the
init's, so its region set differs from root's in two regions: the data window and the stack. The
commit rewrites those two descriptors and leaves the flash one. The thread starts in
`kickos_program_entry`, the start the program's image header names:

```c++
extern "C" void kickos_program_entry(void* args)
{
    auto const a = static_cast<kos_task_args*>(args);
    kickos_program_run(kickos_task_enter(a), a);
}
```

`kickos_task_enter` runs the image's startup, records which declaration the task is (the one
`kos_self` answers) and returns the task's record from the table. `kickos_program_run` calls
the record's program entry, the application's `main`, with the argument vector the create
passed, or the system's where it passed none:

```c++
extern "C" void kickos_program_run(kos_table_task const* t, kos_task_args* a)
{
    int argc = static_cast<int>(a->argc);
    char** argv = a->argv;
    if (argc == 0)
    {
        argc = kickos_init_args.argc;
        argv = kickos_init_args.argv;
    }
    exit(t->entry.program(argc, argv));
}
```

You write an ordinary `int main(int argc, char** argv)`, and the build leaves it named `main`
where a composition runs it as a program. When `main` returns, `exit` runs, and because the
composition says the system `ends: main`, the whole system shuts down with that status. `hello`
returns 0 after its ten rounds.

**Try it:** first the switch into `main`. The set waiting for the commit names the application's
data window at `0x20010000` (32 KiB) and `main`'s stack at `0x2001c000`, where root's named
`0x20018000` and `0x2001a000`; the flash region is the same words.

```
(gdb) break kickos::init::Walk::wake
(gdb) continue
(gdb) break kickos_arch_mpu_commit
(gdb) continue
(gdb) bt
#0  kickos_arch_mpu_commit () at .../arch/arm/common/kickos_arch_mpu_commit_default.cc:22
#1  0x00000ae6 in PendSV_Handler () at .../arch/arm/armv7m/switch.S:281
(gdb) p/x *kickos_arm_mpu_pending()
$1 = {rbar = {0x0, 0x20010000, 0x2001c000, 0x0, 0x0, 0x0, 0x0, 0x0},
      rasr = {0x603002b, 0x1303001d, 0x13030019, 0x0, 0x0, 0x0, 0x0, 0x0}}
```

Then `main` itself:

```
(gdb) break main
(gdb) continue
Breakpoint 3, main () at .../user/apps/common/hello/main.cc:51
(gdb) info registers control psp
control        0x3                 3
psp            0x2001dfd0          536993744
(gdb) x/wx 0x20010004
0x20010004 <_impure_ptr>:	0x2001c000
(gdb) x/2wx 0x2001c178
0x2001c178:	0x2001c000	0x00000002
```

This is the end of the walk: `main`, unprivileged (`CONTROL = 3`), on its own stack, with its
own C library state at the base of its own stack block. The application image's `_impure_ptr`
names it, and its cell carries `main`'s lock id, 2.

The next line calls `kos::print`, which ends in a syscall. That is where
[02](02-first-syscall.md) begins.

**Why, and the contract:** [Reference: `app-entry-via-init-seam` and `init-return-is-shutdown`](../reference/invariants.md#app-entry--init-seam)

## Where to next

- [2. The first syscall](02-first-syscall.md): what happens on `kos::print`.
- [8. Spawn and capabilities](08-spawn-and-caps.md): `kos_task_create` from Stop 16 and
  `kos_thread_create`, in full.
- [10. The MMU](10-the-mmu.md): the same boot on `qemu-arm64`, where root's regions become an
  address space.

## Exercises

1. **A constructor of your own.** Add a global object with a constructor to
   `user/apps/common/hello/main.cc` (for example one that sets a global `int` to 42). Predict:
   which list holds it, which thread runs it, and at what privilege. Check: break on the
   constructor, then `bt` and `info registers control`. You should see `kickos_task_enter` in
   the backtrace and `control = 3`.
2. **The MPU's view.** At Stop 14, `x/wx 0x20018000` (the init's data window) and
   `x/wx 0x20000000` (the kernel's data). Predict which read works. Then do the same two reads
   at Stop 16, inside the syscall, and explain the difference.
3. **Who runs first.** Change the `main` task's `priority` in `boards/qemu/composition.yaml`
   from 2 to 3 and rebuild. Predict whether `main` is now reached before or after
   `Walk::wake`. Check with breakpoints on both.
